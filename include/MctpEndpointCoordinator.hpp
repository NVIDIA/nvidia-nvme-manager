/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <utility>

/**
 * Serialize NVMe-MI commands and an SPDM transaction on one MCTP transport.
 *
 * NVMe-MI calls are synchronous on their worker thread, while an SPDM refresh
 * spans multiple asynchronous request/response exchanges.  A condition
 * variable can therefore gate MI worker calls, but SPDM readiness is delivered
 * by callback so the main event loop is never blocked.
 */
class MctpEndpointCoordinator
{
  private:
    struct PendingSpdmRequest
    {
        std::function<void()> ready;
        std::function<void(std::exception_ptr)> onFailure;
    };

  public:
    void lock()
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [this] { return !spdmActive; });
        ++nvmeMiActive;
    }

    void unlock()
    {
        PendingSpdmRequest request;
        {
            std::lock_guard lock(mutex);
            if (nvmeMiActive == 0)
            {
                return;
            }
            --nvmeMiActive;
            if (nvmeMiActive == 0 && !spdmReady.empty())
            {
                spdmActive = true;
                request = std::move(spdmReady.front());
                spdmReady.pop();
            }
        }

        invokeReady(request);
        condition.notify_all();
    }

    bool requestSpdm(std::function<void()>&& ready,
                     std::function<void(std::exception_ptr)>&& onFailure)
    {
        PendingSpdmRequest runNow;
        {
            std::lock_guard lock(mutex);
            if (!ready)
            {
                return false;
            }

            if (nvmeMiActive == 0 && !spdmActive)
            {
                spdmActive = true;
                runNow = {std::move(ready), std::move(onFailure)};
            }
            else
            {
                spdmReady.push({std::move(ready), std::move(onFailure)});
            }
        }

        invokeReady(runNow);
        return true;
    }

    void releaseSpdm()
    {
        PendingSpdmRequest request;
        {
            std::lock_guard lock(mutex);
            spdmActive = false;
            if (!spdmReady.empty())
            {
                spdmActive = true;
                request = std::move(spdmReady.front());
                spdmReady.pop();
            }
        }
        condition.notify_all();
        invokeReady(request);
    }

  private:
    void invokeReady(const PendingSpdmRequest& request)
    {
        if (!request.ready)
        {
            return;
        }

        try
        {
            request.ready();
        }
        catch (...)
        {
            const std::exception_ptr exception = std::current_exception();
            PendingSpdmRequest nextRequest;
            {
                std::lock_guard lock(mutex);
                spdmActive = false;
                if (!spdmReady.empty())
                {
                    spdmActive = true;
                    nextRequest = std::move(spdmReady.front());
                    spdmReady.pop();
                }
            }
            condition.notify_all();

            std::exception_ptr onFailureException;
            if (request.onFailure)
            {
                try
                {
                    request.onFailure(exception);
                }
                catch (...)
                {
                    onFailureException = std::current_exception();
                }
            }

            invokeReady(nextRequest);

            if (onFailureException)
            {
                std::rethrow_exception(onFailureException);
            }
        }
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::size_t nvmeMiActive{};
    bool spdmActive{};
    std::queue<PendingSpdmRequest> spdmReady;
};
