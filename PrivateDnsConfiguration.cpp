/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "resolv"

#include "PrivateDnsConfiguration.h"

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <netdb.h>
#include <netdutils/ThreadUtil.h>
#include <sys/socket.h>

#include "DnsTlsTransport.h"
#include "ResolverEventReporter.h"
#include "netd_resolv/resolv.h"
#include "netdutils/BackoffSequence.h"
#include "util.h"

using android::base::StringPrintf;
using android::netdutils::setThreadName;
using std::chrono::milliseconds;

namespace android {
namespace net {

std::string addrToString(const sockaddr_storage* addr) {
    char out[INET6_ADDRSTRLEN] = {0};
    getnameinfo((const sockaddr*) addr, sizeof(sockaddr_storage), out, INET6_ADDRSTRLEN, nullptr, 0,
                NI_NUMERICHOST);
    return std::string(out);
}

bool parseServer(const char* server, sockaddr_storage* parsed) {
    addrinfo hints = {
            .ai_flags = AI_NUMERICHOST | AI_NUMERICSERV,
            .ai_family = AF_UNSPEC,
    };
    addrinfo* res;

    int err = getaddrinfo(server, "853", &hints, &res);
    if (err != 0) {
        LOG(WARNING) << "Failed to parse server address (" << server << "): " << gai_strerror(err);
        return false;
    }

    memcpy(parsed, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    return true;
}

int PrivateDnsConfiguration::set(int32_t netId, uint32_t mark,
                                 const std::vector<std::string>& servers, const std::string& name,
                                 const std::string& caCert) {
    LOG(DEBUG) << "PrivateDnsConfiguration::set(" << netId << ", 0x" << std::hex << mark << std::dec
               << ", " << servers.size() << ", " << name << ")";

    // Parse the list of servers that has been passed in
    PrivateDnsTracker tmp;
    for (const auto& s : servers) {
        sockaddr_storage parsed;
        if (!parseServer(s.c_str(), &parsed)) {
            return -EINVAL;
        }
        DnsTlsServer server(parsed);
        server.name = name;
        server.certificate = caCert;
        tmp[ServerIdentity(server)] = server;
    }

    std::lock_guard guard(mPrivateDnsLock);
    if (!name.empty()) {
        mPrivateDnsModes[netId] = PrivateDnsMode::STRICT;
    } else if (!tmp.empty()) {
        mPrivateDnsModes[netId] = PrivateDnsMode::OPPORTUNISTIC;
    } else {
        mPrivateDnsModes[netId] = PrivateDnsMode::OFF;
        mPrivateDnsTransports.erase(netId);
        // TODO: signal validation threads to stop.
        return 0;
    }

    // Create the tracker if it was not present
    auto& tracker = mPrivateDnsTransports[netId];

    // Add the servers if not contained in tracker.
    for (const auto& [identity, server] : tmp) {
        if (tracker.find(identity) == tracker.end()) {
            tracker[identity] = server;
        }
    }

    for (auto& [identity, server] : tracker) {
        const bool active = tmp.find(identity) != tmp.end();
        server.setActive(active);

        // For simplicity, deem the validation result of inactive servers as unreliable.
        if (!server.active() && server.validationState() == Validation::success) {
            updateServerState(identity, Validation::success_but_expired, netId);
        }

        if (needsValidation(server)) {
            updateServerState(identity, Validation::in_process, netId);
            startValidation(server, netId, mark);
        }
    }

    return 0;
}

PrivateDnsStatus PrivateDnsConfiguration::getStatus(unsigned netId) {
    PrivateDnsStatus status{PrivateDnsMode::OFF, {}};
    std::lock_guard guard(mPrivateDnsLock);

    const auto mode = mPrivateDnsModes.find(netId);
    if (mode == mPrivateDnsModes.end()) return status;
    status.mode = mode->second;

    const auto netPair = mPrivateDnsTransports.find(netId);
    if (netPair != mPrivateDnsTransports.end()) {
        for (const auto& [_, server] : netPair->second) {
            if (server.active()) {
                status.serversMap.emplace(server, server.validationState());
            }
        }
    }

    return status;
}

void PrivateDnsConfiguration::clear(unsigned netId) {
    LOG(DEBUG) << "PrivateDnsConfiguration::clear(" << netId << ")";
    std::lock_guard guard(mPrivateDnsLock);
    mPrivateDnsModes.erase(netId);
    mPrivateDnsTransports.erase(netId);
}

void PrivateDnsConfiguration::startValidation(const DnsTlsServer& server, unsigned netId,
                                              uint32_t mark) REQUIRES(mPrivateDnsLock) {
    // Note that capturing |server| and |netId| in this lambda create copies.
    std::thread validate_thread([this, server, netId, mark] {
        setThreadName(StringPrintf("TlsVerify_%u", netId).c_str());

        // cat /proc/sys/net/ipv4/tcp_syn_retries yields "6".
        //
        // Start with a 1 minute delay and backoff to once per hour.
        //
        // Assumptions:
        //     [1] Each TLS validation is ~10KB of certs+handshake+payload.
        //     [2] Network typically provision clients with <=4 nameservers.
        //     [3] Average month has 30 days.
        //
        // Each validation pass in a given hour is ~1.2MB of data. And 24
        // such validation passes per day is about ~30MB per month, in the
        // worst case. Otherwise, this will cost ~600 SYNs per month
        // (6 SYNs per ip, 4 ips per validation pass, 24 passes per day).
        auto backoff = netdutils::BackoffSequence<>::Builder()
                               .withInitialRetransmissionTime(std::chrono::seconds(60))
                               .withMaximumRetransmissionTime(std::chrono::seconds(3600))
                               .build();

        while (true) {
            // ::validate() is a blocking call that performs network operations.
            // It can take milliseconds to minutes, up to the SYN retry limit.
            LOG(WARNING) << "Validating DnsTlsServer on netId " << netId;
            const bool success = DnsTlsTransport::validate(server, netId, mark);
            LOG(DEBUG) << "validateDnsTlsServer returned " << success << " for "
                       << addrToString(&server.ss);

            const bool needs_reeval = this->recordPrivateDnsValidation(server, netId, success);
            if (!needs_reeval) {
                break;
            }

            if (backoff.hasNextTimeout()) {
                // TODO: make the thread able to receive signals to shutdown early.
                std::this_thread::sleep_for(backoff.getNextTimeout());
            } else {
                break;
            }
        }
    });
    validate_thread.detach();
}

bool PrivateDnsConfiguration::recordPrivateDnsValidation(const DnsTlsServer& server, unsigned netId,
                                                         bool success) {
    constexpr bool NEEDS_REEVALUATION = true;
    constexpr bool DONT_REEVALUATE = false;
    const ServerIdentity identity = ServerIdentity(server);

    std::lock_guard guard(mPrivateDnsLock);

    auto netPair = mPrivateDnsTransports.find(netId);
    if (netPair == mPrivateDnsTransports.end()) {
        LOG(WARNING) << "netId " << netId << " was erased during private DNS validation";
        maybeNotifyObserver(identity.ip.toString(), Validation::fail, netId);
        return DONT_REEVALUATE;
    }

    const auto mode = mPrivateDnsModes.find(netId);
    if (mode == mPrivateDnsModes.end()) {
        LOG(WARNING) << "netId " << netId << " has no private DNS validation mode";
        maybeNotifyObserver(identity.ip.toString(), Validation::fail, netId);
        return DONT_REEVALUATE;
    }
    const bool modeDoesReevaluation = (mode->second == PrivateDnsMode::STRICT);

    bool reevaluationStatus =
            (success || !modeDoesReevaluation) ? DONT_REEVALUATE : NEEDS_REEVALUATION;

    auto& tracker = netPair->second;
    auto serverPair = tracker.find(identity);
    if (serverPair == tracker.end()) {
        LOG(WARNING) << "Server " << addrToString(&server.ss)
                     << " was removed during private DNS validation";
        success = false;
        reevaluationStatus = DONT_REEVALUATE;
    } else if (!(serverPair->second == server)) {
        // TODO: It doesn't seem correct to overwrite the tracker entry for
        // |server| down below in this circumstance... Fix this.
        LOG(WARNING) << "Server " << addrToString(&server.ss)
                     << " was changed during private DNS validation";
        success = false;
        reevaluationStatus = DONT_REEVALUATE;
    } else if (!serverPair->second.active()) {
        LOG(WARNING) << "Server " << addrToString(&server.ss)
                     << " was removed from the configuration";
        success = false;
        reevaluationStatus = DONT_REEVALUATE;
    }

    // Send a validation event to NetdEventListenerService.
    const auto& listeners = ResolverEventReporter::getInstance().getListeners();
    if (listeners.size() != 0) {
        for (const auto& it : listeners) {
            it->onPrivateDnsValidationEvent(netId, addrToString(&server.ss), server.name, success);
        }
        LOG(DEBUG) << "Sent validation " << (success ? "success" : "failure") << " event on netId "
                   << netId << " for " << addrToString(&server.ss) << " with hostname {"
                   << server.name << "}";
    } else {
        LOG(ERROR)
                << "Validation event not sent since no INetdEventListener receiver is available.";
    }

    if (success) {
        updateServerState(identity, Validation::success, netId);
    } else {
        // Validation failure is expected if a user is on a captive portal.
        // TODO: Trigger a second validation attempt after captive portal login
        // succeeds.
        const auto result = (reevaluationStatus == NEEDS_REEVALUATION) ? Validation::in_process
                                                                       : Validation::fail;
        updateServerState(identity, result, netId);
    }
    LOG(WARNING) << "Validation " << (success ? "success" : "failed");

    return reevaluationStatus;
}

void PrivateDnsConfiguration::updateServerState(const ServerIdentity& identity, Validation state,
                                                uint32_t netId) {
    auto netPair = mPrivateDnsTransports.find(netId);
    if (netPair == mPrivateDnsTransports.end()) {
        maybeNotifyObserver(identity.ip.toString(), Validation::fail, netId);
        return;
    }

    auto& tracker = netPair->second;
    if (tracker.find(identity) == tracker.end()) {
        maybeNotifyObserver(identity.ip.toString(), Validation::fail, netId);
        return;
    }

    tracker[identity].setValidationState(state);
    maybeNotifyObserver(identity.ip.toString(), state, netId);
}

bool PrivateDnsConfiguration::needsValidation(const DnsTlsServer& server) {
    // The server is not expected to be used on the network.
    if (!server.active()) return false;

    // The server is newly added.
    if (server.validationState() == Validation::unknown_server) return true;

    // The server has failed at least one validation attempt. Give it another try.
    if (server.validationState() == Validation::fail) return true;

    // The previous validation result might be unreliable.
    if (server.validationState() == Validation::success_but_expired) return true;

    return false;
}

void PrivateDnsConfiguration::setObserver(Observer* observer) {
    std::lock_guard guard(mPrivateDnsLock);
    mObserver = observer;
}

void PrivateDnsConfiguration::maybeNotifyObserver(const std::string& serverIp,
                                                  Validation validation, uint32_t netId) const {
    if (mObserver) {
        mObserver->onValidationStateUpdate(serverIp, validation, netId);
    }
}

}  // namespace net
}  // namespace android
