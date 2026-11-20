#include <stdio.h>
#include <stdint.h>

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;

#include "via6522.h"

#define CHECK(expression) do { checks++; if (!(expression)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #expression); } } while (0)

int main(void)
{
	Via6522 via;
	Via6522 other;
	int checks = 0;
	int failures = 0;
	uint8 value;
	Via6522Reset(&via, 0);
	Via6522Reset(&other, 0);
	CHECK(Via6522Read(&via, 0, 0) == 0xff);
	CHECK(Via6522Read(&via, 14, 0) == 0x80);
	Via6522Write(&via, 0, 0x12, 0);
	CHECK(Via6522Read(&via, 0, 0) == 0xff);
	Via6522Write(&via, 2, 0xf0, 0);
	CHECK(Via6522Read(&via, 0, 0) == 0x1f);
	via.inputb = 5;
	CHECK(Via6522Read(&via, 0, 0) == 0x15);
	Via6522Write(&via, 2, 0xff, 0);
	CHECK(Via6522Read(&via, 0, 0) == 0x12);
	CHECK(Via6522Read(&other, 0, 0) == 0xff);
	Via6522Write(&via, 0, 0x90, 0);
	value = Via6522Read(&via, 0, 0);
	Via6522Write(&via, 0, value & 0x7f, 0);
	CHECK(Via6522Read(&via, 0, 0) == 0x10);
	Via6522Write(&via, 3, 0xff, 0);
	Via6522Write(&via, 15, 0x5a, 0);
	CHECK(Via6522Read(&via, 1, 0) == 0x5a);
	via.registers[13] = 3;
	CHECK(Via6522Read(&via, 15, 0) == 0x5a);
	CHECK(Via6522Read(&via, 13, 0) == 3);
	Via6522Read(&via, 1, 0);
	CHECK(Via6522Read(&via, 13, 0) == 0);
	Via6522Write(&via, 14, 0xc0, 0);
	Via6522Write(&via, 14, 0xa0, 0);
	CHECK(Via6522Read(&via, 14, 0) == 0xe0);
	Via6522Write(&via, 14, 0x40, 0);
	CHECK(Via6522Read(&via, 14, 0) == 0xa0);
	Via6522Write(&via, 14, 0xc0, 0);
	Via6522Write(&via, 4, 3, 0);
	Via6522Write(&via, 5, 0, 0);
	CHECK(Via6522Read(&via, 4, 2) == 1);
	CHECK(Via6522Read(&via, 13, 3) == 0);
	CHECK(Via6522Read(&via, 13, 4) == 0xc0);
	CHECK(Via6522Read(&via, 4, 4) == 0xff);
	CHECK(Via6522Read(&via, 13, 4) == 0);
	CHECK(Via6522Read(&via, 13, 0x10005) == 0);
	Via6522Reset(&via, 0);
	Via6522Write(&via, 11, 0xc0, 0);
	Via6522Write(&via, 4, 1, 0);
	Via6522Write(&via, 5, 0, 0);
	CHECK((Via6522Read(&via, 0, 0) & 0x80) == 0);
	CHECK(Via6522Read(&via, 13, 2) == 0x40);
	CHECK((Via6522Read(&via, 0, 2) & 0x80) == 0x80);
	Via6522Write(&via, 13, 0x40, 2);
	CHECK(Via6522Read(&via, 4, 3) == 1);
	CHECK(Via6522Read(&via, 13, 4) == 0);
	CHECK(Via6522Read(&via, 13, 5) == 0x40);
	CHECK((Via6522Read(&via, 0, 5) & 0x80) == 0);
	Via6522Write(&via, 13, 0x40, 5);
	CHECK(Via6522Read(&via, 13, 11) == 0x40);
	CHECK((Via6522Read(&via, 0, 11) & 0x80) == 0);
	Via6522Reset(&via, 0);
	Via6522Write(&via, 4, 10, 0);
	Via6522Write(&via, 5, 0, 0);
	Via6522Write(&via, 6, 100, 2);
	CHECK(Via6522Read(&via, 4, 3) == 7);
	CHECK(Via6522Read(&via, 6, 3) == 100);
	Via6522Write(&via, 8, 1, 3);
	Via6522Write(&via, 9, 0, 3);
	CHECK(Via6522Read(&via, 13, 5) == 0x20);
	Via6522Read(&via, 8, 5);
	CHECK(Via6522Read(&via, 13, 5) == 0);
	Via6522Write(&via, 14, 0xa0, 5);
	Via6522Write(&via, 8, 0, 5);
	Via6522Write(&via, 9, 0, 5);
	CHECK(Via6522Read(&via, 13, 6) == 0xa0);
	Via6522Write(&via, 13, 0x20, 6);
	CHECK(Via6522Read(&via, 13, 6) == 0);
	Via6522Reset(&via, 0);
	CHECK(Via6522Read(&via, 14, 0) == 0x80);
	CHECK(Via6522Read(&via, 13, 0) == 0);
	printf("checks=%d failures=%d\n", checks, failures);
	return failures != 0;
}
