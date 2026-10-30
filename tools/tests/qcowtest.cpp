#include "sysdeps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qcow.h"
#include "chd.h"

#define TESTCHUNK (1024 * 1024)
#define TESTMAXWRITE (300 * 1024)

#if defined(_MSC_VER)
#define TESTSEEK _fseeki64
#else
#define TESTSEEK fseeko
#endif

typedef struct QcowTest {
	uint32 random;
	QcowImage *image;
	ChdImage *chd;
	FILE *raw;
	loff_t size;
	uint8 *expected;
	uint8 *actual;
	int failures;
} QcowTest;

static uint32 Random(QcowTest *test)
{
	test->random ^= test->random << 13;
	test->random ^= test->random >> 17;
	test->random ^= test->random << 5;
	return test->random;
}

static loff_t RandomOffset(QcowTest *test)
{
	uint64 high;
	uint64 low;
	high = Random(test);
	low = Random(test);
	return (loff_t)(((high << 32) | low) % (uint64)test->size);
}

static size_t ImageRead(QcowTest *test, void *buffer, loff_t offset, size_t length)
{
	if (test->chd)
		return ChdRead(test->chd, buffer, offset, length);
	return QcowRead(test->image, buffer, offset, length);
}

static size_t ImageWrite(QcowTest *test, const void *buffer, loff_t offset, size_t length)
{
	if (test->chd)
		return ChdWrite(test->chd, buffer, offset, length);
	return QcowWrite(test->image, buffer, offset, length);
}

static size_t RawRead(QcowTest *test, loff_t offset, size_t length)
{
	if (TESTSEEK(test->raw, (int64)offset, SEEK_SET) != 0)
		return 0;
	return fread(test->expected, 1, length, test->raw);
}

static void Compare(QcowTest *test, loff_t offset, size_t length)
{
	size_t expected;
	size_t actual;
	expected = RawRead(test, offset, length);
	actual = ImageRead(test, test->actual, offset, length);
	if (expected != actual || memcmp(test->expected, test->actual, expected) != 0) {
		test->failures++;
		if (test->failures <= 8)
			printf("FAIL read offset %lld length %u got %u want %u\n", (long long)offset, (unsigned)length, (unsigned)actual, (unsigned)expected);
	}
}

static void WriteBoth(QcowTest *test, loff_t offset, size_t length)
{
	size_t index;
	size_t written;
	for (index = 0; index < length; index++)
		test->expected[index] = (uint8)Random(test);
	written = ImageWrite(test, test->expected, offset, length);
	TESTSEEK(test->raw, (int64)offset, SEEK_SET);
	fwrite(test->expected, 1, written, test->raw);
	if (written != length) {
		test->failures++;
		if (test->failures <= 8)
			printf("FAIL write offset %lld length %u wrote %u\n", (long long)offset, (unsigned)length, (unsigned)written);
	}
}

int main(int argc, char **argv)
{
	QcowTest test;
	loff_t offset;
	size_t length;
	int iterations;
	int index;
	int status;
	if (argc < 4) {
		printf("usage: QcowTest image.qcow2 image.raw iterations [seed]\n");
		return 2;
	}
	memset(&test, 0, sizeof(test));
	test.random = 0x9e3779b9u;
	if (argc > 4)
		test.random = (uint32)strtoul(argv[4], NULL, 0);
	iterations = atoi(argv[3]);
	status = QcowOpen(argv[1], iterations == 0, &test.image);
	if (status == QCOWNOTQCOW)
		status = ChdOpen(argv[1], iterations == 0, &test.chd);
	if (status != QCOWOPENED) {
		printf("FAIL open %s\n", argv[1]);
		return 1;
	}
	test.raw = fopen(argv[2], "r+b");
	if (test.chd)
		test.size = ChdSize(test.chd);
	else
		test.size = QcowSize(test.image);
	test.expected = (uint8 *)malloc(TESTCHUNK);
	test.actual = (uint8 *)malloc(TESTCHUNK);
	if (test.raw == NULL || test.expected == NULL || test.actual == NULL) {
		printf("FAIL setup\n");
		return 1;
	}
	for (index = 0; index < iterations; index++) {
		offset = RandomOffset(&test);
		length = 1 + Random(&test) % TESTMAXWRITE;
		if (Random(&test) & 1) {
			offset &= ~(loff_t)511;
			length = (length + 511) & ~(size_t)511;
		}
		if (offset + (loff_t)length > test.size)
			length = (size_t)(test.size - offset);
		if (Random(&test) % 3 == 0)
			Compare(&test, offset, length);
		else
			WriteBoth(&test, offset, length);
	}
	for (offset = 0; offset < test.size; offset += TESTCHUNK) {
		length = TESTCHUNK;
		if (offset + (loff_t)length > test.size)
			length = (size_t)(test.size - offset);
		Compare(&test, offset, length);
	}
	if (test.chd)
		ChdClose(test.chd);
	else
		QcowClose(test.image);
	fclose(test.raw);
	printf("size %lld iterations %d failures %d\n", (long long)test.size, iterations, test.failures);
	return test.failures != 0;
}
