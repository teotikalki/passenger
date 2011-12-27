#include "TestSupport.h"
#include <boost/make_shared.hpp>
#include <boost/bind.hpp>
#include <agents/HelperAgent/FileBackedPipe.h>
#include <algorithm>
#include <pthread.h>

using namespace Passenger;
using namespace std;
using namespace boost;

namespace tut {
	struct FileBackedPipeTest {
		TempDir tmpdir;
		BackgroundEventLoop bg;
		FileBackedPipePtr pipe;

		bool consumeImmediately;
		size_t toConsume;
		bool doneAfterConsuming;
		pthread_t consumeCallbackThread;
		unsigned int consumeCallbackCount;
		string receivedData;
		bool ended;
		FileBackedPipe::ConsumeCallback consumedCallback;

		FileBackedPipeTest()
			: tmpdir("tmp.pipe")
		{
			consumeImmediately = true;
			toConsume = 9999;
			doneAfterConsuming = false;
			consumeCallbackCount = 0;
			ended = false;
			pipe = make_shared<FileBackedPipe>(bg.safe, "tmp.pipe");
			pipe->onEnd = boost::bind(&FileBackedPipeTest::onEnd, this);
		}
		
		~FileBackedPipeTest() {
			MultiLibeio::waitUntilIdle();
			bg.stop();
		}

		void init() {
			pipe->onData = boost::bind(&FileBackedPipeTest::onData,
				this, _1, _2, _3);
			bg.start();
		}

		bool write(const StaticString &data) {
			bool result;
			bg.safe->run(boost::bind(&FileBackedPipeTest::real_write, this, data, &result));
			return result;
		}

		void real_write(StaticString data, bool *result) {
			*result = pipe->write(data.data(), data.size());
		}

		unsigned int getBufferSize() {
			unsigned int result;
			bg.safe->run(boost::bind(&FileBackedPipeTest::real_getBufferSize, this, &result));
			return result;
		}

		void real_getBufferSize(unsigned int *result) {
			*result = pipe->getBufferSize();
		}

		void startPipe() {
			bg.safe->run(boost::bind(&FileBackedPipe::start, pipe.get()));
		}

		void endPipe() {
			bg.safe->run(boost::bind(&FileBackedPipe::end, pipe.get()));
		}

		void callConsumedCallback(size_t consumed, bool done) {
			bg.safe->run(boost::bind(consumedCallback, consumed, done));
		}

		bool isStarted() {
			bool result;
			bg.safe->run(boost::bind(&FileBackedPipeTest::real_isStarted, this, &result));
			return result;
		}

		void real_isStarted(bool *result) {
			*result = pipe->isStarted();
		}

		FileBackedPipe::DataState getDataState() {
			FileBackedPipe::DataState result;
			bg.safe->run(boost::bind(&FileBackedPipeTest::real_getDataState, this, &result));
			return result;
		}

		void real_getDataState(FileBackedPipe::DataState *result) {
			*result = pipe->getDataState();
		}

		void onData(const char *data, size_t size, const FileBackedPipe::ConsumeCallback &consumed) {
			consumeCallbackThread = pthread_self();
			consumeCallbackCount++;
			if (!receivedData.empty()) {
				receivedData.append("\n");
			}
			receivedData.append(data, size);
			if (consumeImmediately) {
				consumed(std::min(toConsume, size), doneAfterConsuming);
			} else {
				consumedCallback = consumed;
			}
		}

		void onEnd() {
			ended = true;
		}
	};

	DEFINE_TEST_GROUP(FileBackedPipeTest);

	TEST_METHOD(1) {
		// Test writing to an empty, started pipe and consuming all data immediately.
		init();
		startPipe();
		ensure("immediately consumed", write("hello"));
		ensure("callback called from event loop thread",
			pthread_equal(consumeCallbackThread, bg.safe->getCurrentThread()));
		ensure_equals(receivedData, "hello");
		ensure_equals("nothing buffered", getBufferSize(), 0u);
	}

	TEST_METHOD(2) {
		// Test writing to an empty, started pipe and not consuming immediately.
		init();
		startPipe();
		consumeImmediately = false;
		ensure("not immediately consumed", !write("hello"));
		ensure_equals(receivedData, "hello");
		ensure_equals("everything buffered", getBufferSize(), sizeof("hello") - 1);

		receivedData.clear();
		callConsumedCallback(5, false);
		ensure_equals(getBufferSize(), 0u);
	}

	TEST_METHOD(3) {
		// When the consume callback is called with done=false, the pipe should be paused.
		init();
		startPipe();
		doneAfterConsuming = true;
		write("hello");
		ensure(!isStarted());
		ensure_equals(getBufferSize(), 0u);
	}

	TEST_METHOD(4) {
		// After consuming some data, if the pipe is still in started mode then
		// it should emit any remaining data.
		init();
		startPipe();
		toConsume = 3;
		write("hello");
		ensure_equals(getBufferSize(), 0u);
		ensure_equals(receivedData,
			"hello\n"
			"lo");
		ensure_equals(consumeCallbackCount, 2u);
	}

	TEST_METHOD(5) {
		// Writing to a stopped pipe will cause the data to be buffer.
		// This buffer will be passed to the data callback when we
		// start the pipe again. If the data callback doesn't consume
		// everything at once then the pipe will try again until
		// everything's consumed.
		init();
		toConsume = 3;
		write("hello");
		ensure_equals(getBufferSize(), 5u);
		ensure_equals(receivedData, "");
		ensure_equals(consumeCallbackCount, 0u);
		startPipe();
		ensure_equals(getBufferSize(), 0u);
		ensure_equals(consumeCallbackCount, 2u);
		ensure_equals(receivedData,
			"hello\n"
			"lo");
	}

	TEST_METHOD(6) {
		// When the data doesn't fit in the memory buffer it will
		// write to a file. Test whether writing to the file and
		// reading from the file works correctly.
		pipe->setThreshold(5);
		init();
		write("hello");
		ensure_equals(getBufferSize(), 5u);
		ensure_equals(getDataState(), FileBackedPipe::IN_MEMORY);
		write("world");
		ensure_equals(getBufferSize(), 10u);
		usleep(25000);
		ensure_equals(getBufferSize(), 10u);
		ensure_equals(getDataState(), FileBackedPipe::IN_FILE);
		startPipe();
		usleep(25000);
		ensure_equals(getBufferSize(), 0u);
		ensure_equals(receivedData, "helloworld");
	}

	TEST_METHOD(7) {
		// Test end() on a started, empty pipe.
		init();
		startPipe();
		endPipe();
		ensure_equals(consumeCallbackCount, 0u);
		ensure(ended);
	}

	TEST_METHOD(8) {
		// Test end() on a started pipe after writing data to
		// it that's immediately consumed.
		init();
		startPipe();
		write("hello");
		endPipe();
		ensure_equals(consumeCallbackCount, 1u);
		ensure_equals(receivedData, "hello");
		ensure(ended);
	}

	TEST_METHOD(9) {
		// Test end() on a started pipe that has data buffered in memory.
		init();
		consumeImmediately = false;
		startPipe();
		write("hello");
		endPipe();
		ensure_equals(getDataState(), FileBackedPipe::IN_MEMORY);
		ensure(!ended);

		callConsumedCallback(3, false);
		ensure_equals(receivedData,
			"hello\n"
			"lo");
		ensure(!ended);
		callConsumedCallback(2, false);
		ensure(ended);
	}

	TEST_METHOD(10) {
		// Test end() on a started pipe that has data buffered on disk.
		init();
		consumeImmediately = false;
		pipe->setThreshold(1);
		startPipe();
		write("hello");
		endPipe();
		usleep(25000);
		ensure_equals(getDataState(), FileBackedPipe::IN_FILE);
		ensure(!ended);

		callConsumedCallback(3, false);
		usleep(25000);
		ensure_equals(receivedData,
			"hello\n"
			"lo");
		ensure(!ended);
		callConsumedCallback(2, false);
		ensure(ended);
	}
}
