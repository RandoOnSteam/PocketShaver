#include <LowMem.h>
#include <OSUtils.h>
#include <stdio.h>

int main(void)
{
	volatile unsigned char *mainbuffer;
	volatile unsigned char *alternate;
	volatile unsigned char *port;
	unsigned char saved;
	unsigned long ticks;
	unsigned int sample;
	FILE *result;
	mainbuffer = (volatile unsigned char *)LMGetSoundBase();
	alternate = mainbuffer - 0x5c00;
	port = (volatile unsigned char *)LMGetVIA() + 0x1e00;
	saved = *port;
	result = fopen("This PC:SoundBaseResult.txt", "w");
	if (result) {
		fprintf(result, "main=%p alternate=%p port=%p saved=%u\n", mainbuffer, alternate, port, saved);
		fflush(result);
	}
	for (sample = 0; sample < 370; sample++) {
		mainbuffer[sample * 2] = 96;
		if (sample % 37 < 18)
			mainbuffer[sample * 2] = 160;
		alternate[sample * 2] = 96;
		if (sample % 74 < 37)
			alternate[sample * 2] = 160;
	}
	*port = saved | 8;
	Delay(120, &ticks);
	*port = saved & ~8;
	Delay(120, &ticks);
	*port = saved | 8;
	Delay(120, &ticks);
	for (sample = 0; sample < 370; sample++) {
		mainbuffer[sample * 2] = 128;
		alternate[sample * 2] = 128;
	}
	*port = saved;
	if (result) {
		fprintf(result, "main/alternate/main complete\n");
		fclose(result);
	}
	return 0;
}
