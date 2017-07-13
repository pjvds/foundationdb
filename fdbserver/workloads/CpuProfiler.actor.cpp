/*
 * CpuProfiler.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "flow/actorcompiler.h"
#include "fdbserver/TesterInterface.h"
#include "fdbserver/QuietDatabase.h"
#include "fdbserver/ServerDBInfo.h"
#include "workloads.h"

//A workload which starts the CPU profiler at a given time and duration on all workers in a cluster
struct CpuProfilerWorkload : TestWorkload
{
	bool success;

	//How long to run the workload before starting the profiler
	double initialDelay;

	//How long the profiler should be run; if <= 0 then it will run until the workload's check function is called
	double duration;

	//A list of worker interfaces which have had profiling turned on
	std::vector<WorkerInterface> profilingWorkers;

	CpuProfilerWorkload(WorkloadContext const& wcx)
		: TestWorkload(wcx)
	{
		initialDelay = getOption(options, LiteralStringRef("initialDelay"), 0.0);
		duration = getOption(options, LiteralStringRef("duration"), -1.0);
		success = true;
	}

	virtual std::string description()
	{
		return "CpuProfiler";
	}

	virtual Future<Void> setup(Database const& cx)
	{
		return Void();
	}

	//Turns the profiler on or off
	ACTOR Future<Void> updateProfiler(bool enabled, Database cx, CpuProfilerWorkload *self)
	{
		if(self->clientId == 0)
		{
			//If we are turning the profiler on, get a list of workers in the system
			if(enabled)
			{
				vector<std::pair<WorkerInterface, ProcessClass>> _workers = wait( getWorkers( self->dbInfo ) );
				vector<WorkerInterface> workers;
				for(int i = 0; i < _workers.size(); i++)
					workers.push_back(_workers[i].first);
				self->profilingWorkers = workers;
			}

			state std::vector<Future<ErrorOr<Void>>> replies;
			state int i;
			//Send a ProfilerRequest to each worker
			for(i = 0; i < self->profilingWorkers.size(); i++)
			{
				ProfilerRequest req;
				req.enabled = enabled;

				//The profiler output name will be the ip.port.prof
				req.outputFile = StringRef(toIPString(self->profilingWorkers[i].address().ip) + "." + format("%d", self->profilingWorkers[i].address().port) + ".prof");

				replies.push_back(self->profilingWorkers[i].clientInterface.cpuProfilerRequest.tryGetReply(req));
			}

			Void _ = wait(waitForAll(replies));

			//Check that all workers succeeded if turning the profiler on
			if(enabled)
				for(i = 0; i < replies.size(); i++)
					if(!replies[i].get().present())
						self->success = false;

			TraceEvent("DoneSignalingProfiler");
		}

		//Enable (or disable) the profiler on the current tester
		ProfilerRequest req;
		req.enabled = enabled;
		req.outputFile = StringRef(toIPString(g_network->getLocalAddress().ip) + "." + format("%d", g_network->getLocalAddress().port) + ".prof");

		updateCpuProfiler(req);

		return Void();
	}

	virtual Future<Void> start(Database const& cx)
	{
		return _start(cx, this);
	}

	ACTOR Future<Void> _start(Database cx, CpuProfilerWorkload *self)
	{
		Void _ = wait(delay(self->initialDelay));
		if(self->clientId == 0)
			TraceEvent("SignalProfilerOn");
		Void _ = wait(timeoutError(self->updateProfiler(true, cx, self), 60.0));

		//If a duration was given, let the duration elapse and then shut the profiler off
		if(self->duration > 0)
		{
			Void _ = wait(delay(self->duration));
			if(self->clientId == 0)
				TraceEvent("SignalProfilerOff");
			Void _ = wait(timeoutError(self->updateProfiler(false, cx, self), 60.0));
		}

		return Void();
	}

	virtual Future<bool> check(Database const& cx)
	{
		return _check(cx, this);
	}

	ACTOR Future<bool> _check(Database cx, CpuProfilerWorkload *self)
	{
		//If no duration was given, then shut the profiler off now
		if(self->duration <= 0)
		{
			if(self->clientId == 0)
				TraceEvent("SignalProfilerOff");
			Void _ = wait(timeoutError(self->updateProfiler(false, cx, self), 60.0));
		}

		return self->success;
	}

	virtual void getMetrics( vector<PerfMetric>& m )
	{

	}
};

WorkloadFactory<CpuProfilerWorkload> CpuProfilerWorkloadFactory("CpuProfiler");
