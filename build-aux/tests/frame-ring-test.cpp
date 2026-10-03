#include "core/frame-ring.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <thread>

extern "C" void *bmalloc(size_t size)
{
	return std::malloc(size);
}
extern "C" void bfree(void *ptr)
{
	std::free(ptr);
}

static RingConfig config()
{
	RingConfig value;
	value.width = 4;
	value.height = 2;
	value.format = VIDEO_FORMAT_NV12;
	value.linesize[0] = value.linesize[1] = 4;
	value.fps = 4;
	value.duration_sec = 2;
	return value;
}

static void produce(FrameRing &ring, uint64_t timestamp)
{
	uint8_t pixels[8];
	for (auto &pixel : pixels)
		pixel = static_cast<uint8_t>(ring.head());
	const uint8_t *planes[MAX_AV_PLANES] = {pixels, pixels};
	uint32_t strides[MAX_AV_PLANES] = {4, 4};
	ring.write(planes, strides, timestamp, false);
}

static void fill(FrameRing &ring)
{
	assert(ring.reconfigure(config()));
	for (uint64_t i = 0; i < 6; ++i)
		produce(ring, i * 1000);
}

static void expect_frame(FrameRing &ring, uint64_t seq)
{
	FrameMeta meta;
	const uint8_t *planes[MAX_AV_PLANES] = {};
	assert(ring.read(seq, meta, planes));
	assert(meta.seq == seq);
	assert(meta.timestamp == seq * 1000);
	assert(planes[0][0] == seq);
	assert(planes[1][3] == seq);
}

static void delayed_two_camera_export()
{
	FrameRing cameras[2];
	int tokens[2];
	for (int i = 0; i < 2; ++i) {
		fill(cameras[i]);
		tokens[i] = cameras[i].acquire_floor(2);
		assert(tokens[i] >= 0);
	}
	// Camera 2 waits through the entire slow export of camera 1.
	for (int camera = 0; camera < 2; ++camera) {
		for (uint64_t seq = 2; seq < 6; ++seq) {
			for (int incoming = 0; incoming < 100; ++incoming)
				for (auto &ring : cameras)
					produce(ring, 100000 + incoming);
			expect_frame(cameras[camera], seq);
			cameras[camera].update_floor(tokens[camera], seq + 1);
		}
		assert(cameras[camera].frames_held_back() > 0);
		cameras[camera].release_floor(tokens[camera]);
		const auto before = cameras[camera].head();
		produce(cameras[camera], 999999);
		assert(cameras[camera].head() == before + 1);
		assert(cameras[camera].gap_seq() == before);
	}
}

static void first_wrap_and_capacity()
{
	FrameRing ring;
	fill(ring);
	int tokens[4];
	for (int &token : tokens) {
		token = ring.acquire_floor(0);
		assert(token >= 0);
	}
	assert(ring.acquire_floor(0) == -1);
	assert(ring.acquire_floor(ring.head()) == -1);
	for (int i = 0; i < 100; ++i)
		produce(ring, 10000 + i);
	expect_frame(ring, 0);
	for (int token : tokens)
		ring.release_floor(token);
	for (int i = 0; i < 20; ++i)
		produce(ring, 20000 + i);
	assert(ring.acquire_floor(0) == -1);
	const auto generation = ring.generation();
	fill(ring);
	assert(ring.generation() != generation);
	assert(ring.acquire_floor(0) >= 0);
}

static void concurrent_slow_reader()
{
	FrameRing ring;
	fill(ring);
	const int token = ring.acquire_floor(2);
	assert(token >= 0);
	std::thread writer([&] {
		for (int i = 0; i < 20000; ++i)
			produce(ring, 100000 + i);
	});
	for (uint64_t seq = 2; seq < 6; ++seq) {
		for (int i = 0; i < 1000; ++i)
			expect_frame(ring, seq);
		ring.update_floor(token, seq + 1);
	}
	writer.join();
	ring.release_floor(token);
}

int main()
{
	delayed_two_camera_export();
	first_wrap_and_capacity();
	concurrent_slow_reader();
	std::cout << "Frame ring regression tests passed\n";
}
