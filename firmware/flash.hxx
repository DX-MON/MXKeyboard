#ifndef FLASH__HXX
#define FLASH__HXX

#include <cstring>
#include <array>
#include <avr/io.h>
//#include <avr/pgmspace.h>

template<typename T> struct flash_t final
{
private:
	T value_;

public:
	constexpr flash_t(const T value) noexcept : value_{value} { }

	operator T() const noexcept
	{
		T result{};
		const uint8_t x{RAMPX};
		const uint8_t z{RAMPZ};

		__asm__(R"(
			mov r26, %A[result]
			mov r27, %B[result]
			out 0x39, %C[result]
			mov r30, %A[value]
			mov r31, %B[value]
			out 0x3B, %C[value]
			ldi r17, %[count]
			cpi r17, 0
loop%=:
			breq loopDone%=
			elpm r16, Z+
			st X+, r16
			dec r17
			rjmp loop%=
loopDone%=:
			)" : [result] "=g" (result) : [value] "p" (&value_), [count] "I" (sizeof(T)) :
				"r16", "r17", "r26", "r27", "r30", "r31"
		);

		RAMPZ = z;
		RAMPX = x;
		return result;
	}
};

template<> struct flash_t<uint16_t> final
{
private:
	uint16_t value_;

public:
	constexpr flash_t(const uint16_t value) noexcept : value_{value} { }
	//operator uint16_t() const noexcept { return pgm_read_word_far(&value_); }
	operator uint16_t() const noexcept
	{
		uint16_t result{};
		__asm__ __volatile__(R"(
			in r0, %[rampz]
			out %[rampz], %C[value]
			movw r30, %[value]
			elpm %A[result], Z+
			elpm %B[result], Z
			out %[rampz], r0
			)" :
			[result] "=r" (result) :
			[value] "p" (&value_), [rampz] "I" (_SFR_IO_ADDR(RAMPZ)) :
			"r30", "r31"
		);
		return result;
	}
};

#endif /*FLASH__HXX*/