/*
 *  File: WatchDog.hpp
 *  Copyright (c) 2023 Florian Porrmann
 *
 *  MIT License
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 */

#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "../../internal/Constants.hpp"
#include "../../internal/Exceptions.hpp"
#include "../../internal/Logger.hpp"
#include "../../internal/Timer.hpp"
#include "../../internal/Types.hpp"
#include "../../internal/UserInterruptBase.hpp"

#ifndef EMBEDDED_XILINX
#include <atomic>
#include <chrono>
#include <mutex>
#endif

namespace clap
{
namespace internal
{
static std::exception_ptr g_pExcept = nullptr;
static int64_t g_pollSleepTimeMS    = 10;

// TODO: Rename to indicate that this also controls whether the thread should be terminated
// TODO: Replace the bool with an enum
using WatchDogFinishCallback = std::function<bool(void)>;

// TODO: Calling WaitForInterrupt with a non-infinit timeout and checking the threadDone flag is not the best solution.
//       Find a better way, i.e., a way to interrupt the call to poll (ppoll or epoll might be a solution)

#ifndef EMBEDDED_XILINX
static void waitForFinishThread(UserInterruptBase* pUserIntr, HasStatus* pStatus, Timer* pTimer, [[maybe_unused]] std::condition_variable* pCv, [[maybe_unused]] const std::string& name,
								std::atomic<bool>* pThreadDone, [[maybe_unused]] const bool& dontTerminate, [[maybe_unused]] const WatchDogFinishCallback& callback)
{
	pThreadDone->store(false, std::memory_order_release);
	pTimer->Start();

	bool end = !dontTerminate;

	if (pUserIntr->IsSet())
		CLAP_LOG_DEBUG << "[" << name << "] Interrupt Mode ... " << std::endl;
	else
		CLAP_LOG_DEBUG << "[" << name << "] Polling Mode ... " << std::endl;

	try
	{
		do
		{
			if (pUserIntr->IsSet())
			{
				while (!pThreadDone->load(std::memory_order_acquire) && !pUserIntr->WaitForInterrupt(100))
					;
			}
			else if (pStatus)
			{
				while (!pThreadDone->load(std::memory_order_acquire) && !pStatus->PollDone())
					std::this_thread::sleep_for(std::chrono::milliseconds(g_pollSleepTimeMS));
			}

			const bool forceTerminate = pThreadDone->load(std::memory_order_acquire);
			if (!forceTerminate && callback)
				end = callback();

		} while (!pThreadDone->load(std::memory_order_acquire) && dontTerminate && !end);
	}
	catch (...)
	{
		// Set the global exception pointer in case of an exception
		g_pExcept = std::current_exception();
	}

	pTimer->Stop();

	pThreadDone->store(true, std::memory_order_release);
	pCv->notify_all();

	CLAP_LOG_DEBUG << "[" << name << "] Finished" << std::endl;
}
#endif

class WatchDog
{
	DISABLE_COPY_ASSIGN_MOVE(WatchDog)

public:
	WatchDog(const std::string& name, UserInterruptPtr pInterrupt) :
		m_name(name),
		m_pInterrupt(std::move(pInterrupt)),
		m_timer()
#ifndef EMBEDDED_XILINX
		,
		m_mtx(),
		m_waitThread(),
		m_cv(),
		m_threadDone(false)
#endif
	{
	}

	~WatchDog()
	{
		Stop();
	}

	operator bool() const
	{
		return (m_pInterrupt->IsSet() || m_pStatus != nullptr);
	}

	void SetUserInterrupt(UserInterruptPtr pInterrupt)
	{
		m_pInterrupt->TransferCallbacks(pInterrupt.get());
		m_pInterrupt = std::move(pInterrupt);
	}

	void InitInterrupt([[maybe_unused]] const uint32_t& devNum, [[maybe_unused]] const uint32_t& interruptNum, [[maybe_unused]] HasInterrupt* pReg = nullptr)
	{
#ifdef _WIN32
		CLAP_LOG_ERROR << CLASS_TAG("WatchDog") << "Error: Interrupts are not supported on Windows." << std::endl;
#else
		m_pInterrupt->Init(devNum, interruptNum, pReg);
		// Check for existing interrupts and clear them
		CLAP_LOG_DEBUG << CLASS_TAG("WatchDog") << "Clearing existing interrupts ..." << std::endl;
		while (m_pInterrupt->WaitForInterrupt(1))
			;

#endif
	}

	void UnsetInterrupt()
	{
#ifdef _WIN32
		CLAP_LOG_ERROR << CLASS_TAG("WatchDog") << "Error: Interrupts are not supported on Windows." << std::endl;
#else
		m_pInterrupt->Unset();
#endif
	}

	void SetStatusRegister(HasStatus* pStatus)
	{
		m_pStatus = pStatus;
	}

	void UnsetStatusRegister()
	{
		m_pStatus = nullptr;
	}

	bool Start(const bool& dontTerminate = false)
	{
#ifndef EMBEDDED_XILINX
		// Check if the thread has finished but was not joined
		if (m_threadDone.load(std::memory_order_acquire))
			joinThread();

		if (m_threadRunning) return false;

		if (!m_pInterrupt->IsSet() && m_pStatus == nullptr)
		{
			std::stringstream ss("");
			ss << CLASS_TAG("WatchDog") << "Error: Trying to start WatchDog thread with neither the interrupt nor the status register set.";
			throw WatchDogException(ss.str());
		}

		g_pExcept = nullptr;
		m_threadDone.store(false, std::memory_order_release);
		m_waitThread    = std::thread(waitForFinishThread, m_pInterrupt.get(), m_pStatus, &m_timer, &m_cv, m_name, &m_threadDone, dontTerminate, m_callback);
		m_threadRunning = true;
#endif

		return true;
	}

	void Stop()
	{
#ifndef EMBEDDED_XILINX
		if (!m_threadRunning) return;

		m_threadDone.store(true, std::memory_order_release);
		m_cv.notify_all();
		joinThread();
		checkException();
#endif
	}

	bool WaitForFinish(const int32_t& timeoutMS = WAIT_INFINITE)
	{
#ifndef EMBEDDED_XILINX
		using namespace std::chrono_literals;

		CLAP_LOG_DEBUG << CLASS_TAG("WatchDog") << "Core=" << m_name << " timeoutMS=" << (timeoutMS == WAIT_INFINITE ? "Infinite" : std::to_string(timeoutMS)) << std::endl;

		if (!m_threadRunning)
			return true;

		if (m_threadDone.load(std::memory_order_acquire))
		{
			joinThread();
			checkException();
			return true;
		}

		{
			std::unique_lock<std::mutex> lck(m_mtx);

			if (timeoutMS == WAIT_INFINITE)
				m_cv.wait(lck, [this] { return m_threadDone.load(std::memory_order_acquire); });
			else if (m_cv.wait_for(lck, std::chrono::milliseconds(timeoutMS)) == std::cv_status::timeout)
				return false;
		}

		joinThread();
		checkException();
#else
		if (m_pInterrupt->IsSet())
		{
			while (!m_pInterrupt->WaitForInterrupt())
				usleep(1);
		}
		else if (m_pStatus != nullptr)
		{
			while (!m_pStatus->PollDone())
				usleep(1);
		}

		m_callback();
#endif

		return true;
	}

	double GetRuntime() const
	{
		return m_timer.GetElapsedTimeInMilliSec();
	}

	void RegisterInterruptCallback(const IntrCallback& callback)
	{
		m_pInterrupt->RegisterCallback(callback);
	}

	void SetFinishCallback(WatchDogFinishCallback callback)
	{
		m_callback = callback;
	}

private:
	void checkException()
	{
		if (g_pExcept)
		{
			try
			{
				std::rethrow_exception(g_pExcept);
			}
			catch ([[maybe_unused]] const std::exception& ex)
			{
			}
		}
	}

	void joinThread()
	{
#ifndef EMBEDDED_XILINX
		std::lock_guard<std::mutex> lck(m_mtx);
		if (!m_threadRunning) return;

		if (m_waitThread.joinable())
			m_waitThread.join();

		m_threadRunning = false;
#endif
	}

private:
	std::string m_name;
	UserInterruptPtr m_pInterrupt;
	Timer m_timer;
#ifndef EMBEDDED_XILINX
	std::mutex m_mtx;
	std::thread m_waitThread;
	std::condition_variable m_cv;
	bool m_threadRunning = false;
	std::atomic<bool> m_threadDone;
#endif
	WatchDogFinishCallback m_callback = nullptr;
	HasStatus* m_pStatus              = nullptr;
};
} // namespace internal

static inline void SetWatchDogPollSleepTimeMS(const uint32_t& timeMS = 10)
{
	internal::g_pollSleepTimeMS = timeMS;
}

} // namespace clap