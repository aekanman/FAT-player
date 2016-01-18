

#include <stdio.h>
#include <unistd.h>

#include "system.h"
#include "sys/alt_irq.h"
#include "basic_io.h"
#include "LCD.h"
#include "SD_Card.h"
#include "fat.h"
#include "wm8731.h"

#include "altera_avalon_timer_regs.h"
#include "altera_avalon_pio_regs.h"


//Global Variables
int bytePerCluster, clusterLength, secCount, sectorSize;
BYTE buffer[512] = {0};
data_file returnData;
int cc[3500];

volatile int stop_flag;
volatile int edge_capture;
volatile int play_type;
volatile int delay_cnt, delay_flag;
volatile BYTE rightBuffer[88200] = {0};

#ifdef BUTTON_PIO_BASE
static void BUTTON_ISR(void* context, alt_u32 id)
{
    if(stop_flag == 0)
    	stop_flag = 1;
    else
    	stop_flag = 0;

    /* Store the value in the Button's edge capture register in *context. */
    edge_capture = IORD_ALTERA_AVALON_PIO_EDGE_CAP(BUTTON_PIO_BASE);

    /* Reset the Button's edge capture register. */
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(BUTTON_PIO_BASE, 0);

}
#endif



/* Initialize the button_pio. */

static void init_button_pio()
{
    /* initialize the push button interrupt vector */
    alt_irq_register( BUTTON_PIO_IRQ, (void*) 0, BUTTON_ISR);

    /* Reset the edge capture register. */
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(BUTTON_PIO_BASE, 0x0);

    /* Enable all 4 button interrupts. */
    IOWR_ALTERA_AVALON_PIO_IRQ_MASK(BUTTON_PIO_BASE, 0xf);

    /* Initialize Variables */
    edge_capture = 0;

}


void write_to_codec(BYTE * buffer, int sectorSize)
{
	UINT16 tmp;
	int i;

	for (i = 0; i < sectorSize; i+=2){
		while(IORD(AUD_FULL_BASE,0)) {} // Wait until FIFO is ready
		tmp = (buffer[i+1] << 8) | (buffer[i]);
		IOWR(AUDIO_0_BASE, 0 , tmp);
	}
}

void write_to_codec_half (BYTE *buffer, int sectorSize)
{
	UINT16 tmp;
	int i;

	for (i = 0; (i+2) < sectorSize; i+=2){
		while(IORD(AUD_FULL_BASE,0)) {} // Wait until FIFO is ready
		tmp = (buffer[i+1] << 8) | (buffer[i]);
		IOWR(AUDIO_0_BASE, 0 , tmp);

		while(IORD(AUD_FULL_BASE,0)) {} // Wait until FIFO is ready
		tmp = (buffer[i+3] << 8) | (buffer[i + 2]);
		IOWR(AUDIO_0_BASE, 0 , tmp);
	}

}

void write_to_codec_double(BYTE * buffer, int sectorSize)
{
	UINT16 tmp;
	int i;

	for (i = 0; (i+4) < sectorSize; i+=8){
		while(IORD(AUD_FULL_BASE,0)) {} // Wait until FIFO is ready
		tmp = (buffer[i+1] << 8) | (buffer[i]);
		IOWR(AUDIO_0_BASE, 0 , tmp);
		while(IORD(AUD_FULL_BASE,0)) {} // Wait until FIFO is ready
		tmp = (buffer[i+3] << 8) | (buffer[i+2]);
		IOWR(AUDIO_0_BASE, 0 , tmp);
	}
}

void write_to_codec_reverse(BYTE * buffer, int sectorSize)
{
	UINT16 tmp;
	int i;

	for (i = sectorSize-1; i = 0; i-=2){
		while(IORD(AUD_FULL_BASE,0)) {} // Wait until FIFO is ready
		tmp = (buffer[i] << 8) | (buffer[i+1]);
		IOWR(AUDIO_0_BASE, 0 , tmp);
	}
}

void write_to_codec_delay(BYTE * buffer, int sectorSize){

	UINT16 tmp;
	int i;

	for (i = 0; (i+4) < sectorSize; i+=4){

		rightBuffer[delay_cnt] = buffer[i+2];
		rightBuffer[delay_cnt+1] = buffer[i+3];

		delay_cnt += 2;

		if ((delay_cnt + 1) >= 88200){
			delay_cnt = 0;
		}
		tmp = ((buffer[i+1] << 8) | (buffer[i]));
		while(IORD(AUD_FULL_BASE,0)) {} // Wait until FIFO is ready
		IOWR(AUDIO_0_BASE, 0 , tmp);

		tmp = ((rightBuffer[delay_cnt+1] << 8) | (rightBuffer[delay_cnt]));
		while(IORD(AUD_FULL_BASE,0)) {} // Wait until FIFO is ready
		IOWR(AUDIO_0_BASE, 0 , tmp);


	}

	// Play End of Buffer
	if (sectorSize != 512){
		for (i = delay_cnt; i < 88200; i+=2){
			while(IORD(AUD_FULL_BASE,0)) {} // Wait until FIFO is ready
			IOWR(AUDIO_0_BASE, 0 , 0x0000);

			while(IORD(AUD_FULL_BASE,0)) {} // Wait until FIFO is ready
			tmp = (rightBuffer[i+1] << 8) | (rightBuffer[i]);
			IOWR(AUDIO_0_BASE, 0 , tmp);
		}
	}
}


void printLCD(char *first, char * second){
	LCD_Init();
	LCD_Show_Text(first);
	LCD_Line2();
	LCD_Show_Text(second);
}

void init_music(){
	int clusterLength, secCount;
	search_for_filetype("WAV", &returnData,0,1);
	printLCD(returnData.Name, "Buffering...");
	clusterLength = 1+ ceil( (float) returnData.FileSize / bytePerCluster);
	build_cluster_chain(cc, clusterLength, &returnData);
	play_type = IORD(SWITCH_PIO_BASE, 0) & 0x07;
	LCD_Display(returnData.Name, play_type);
}

void play_music(int playType){

	/* Play Types
	 * 0 = Normal
	 * 1 = Double
	 * 2 = Half
	 * 3 = Delay
	 * 4 = Reverse
	 */
	int sectorSize;
	secCount = playType == 4 ? (floor( (float) returnData.FileSize/512) - 1) : 0;
	sectorSize = get_rel_sector(&returnData, buffer, cc, secCount);

	while(sectorSize != -1){

		if(stop_flag)
			break;

		if(sectorSize == 0){
			if(playType == 0 || playType == 4)
				write_to_codec(buffer,BPB_BytsPerSec);
			else if(playType == 1)
				write_to_codec_double(buffer,BPB_BytsPerSec);
			else if(playType == 2)
				write_to_codec_half(buffer,BPB_BytsPerSec);
			else{
				UINT16 tmp;
				int i;

				for (i = 0; (i+4) < BPB_BytsPerSec; i+=4){

					rightBuffer[delay_cnt] = buffer[i+2];
					rightBuffer[delay_cnt+1] = buffer[i+3];

					delay_cnt += 2;

					if ((delay_cnt + 1) >= 88200){
						delay_cnt = 0;
					}
					tmp = ((buffer[i+1] << 8) | (buffer[i]));
					while(IORD(AUD_FULL_BASE,0)) {} // Wait until FIFO is ready
					IOWR(AUDIO_0_BASE, 0 , tmp);

					tmp = ((rightBuffer[delay_cnt+1] << 8) | (rightBuffer[delay_cnt]));
					while(IORD(AUD_FULL_BASE,0)) {} // Wait until FIFO is ready
					IOWR(AUDIO_0_BASE, 0 , tmp);


				}
			}
		}

		else{
			if(playType == 0 || playType == 4)
				write_to_codec(buffer,sectorSize);
			else if(playType == 1)
				write_to_codec_double(buffer,sectorSize);
			else if(playType == 2)
				write_to_codec_half(buffer,sectorSize);
			else{
				printf("Playing End of Delay");
				write_to_codec_delay(buffer,sectorSize);
			}

		}
		secCount = playType == 4 ? secCount - 1 : secCount + 1 ;

		sectorSize = get_rel_sector(&returnData, buffer, cc, secCount);
	}

	// Reset stop_flag
	if (stop_flag == 1)
		stop_flag = 0;
	else
		stop_flag = 1;
}

int main()
{
  //Initialize Functions
  SD_card_init();
  init_mbr();
  init_bs();
  init_audio_codec();

  //Setup Push Buttons
  init_button_pio();


  // Initialize Variables
  bytePerCluster = BPB_BytsPerSec * BPB_SecPerClus;

  //play_music(0 , 1);
  //play_music(0 , 2);
  //play_music(0 , 3);
  //play_music(5 , 4);

  stop_flag = 1;
  file_number = 0;
  init_music();
  delay_cnt = 0;
  delay_flag = 0;

  while(1){
	  if(stop_flag == 0){

		  if(edge_capture == 0x01){
			  //stop music
			  stop_flag = 1;
		  }

		  else if(edge_capture == 0x02){
			  // play music
			  delay_cnt = 0;
			  delay_flag = 0;
			  play_type = IORD(SWITCH_PIO_BASE, 0) & 0x07;
			  LCD_Display(returnData.Name, play_type);
			  play_music(play_type);
		  }

		  else if(edge_capture == 0x04){
			  printf("File Number is %d\n",file_number);
			  init_music();
			  usleep(250000);
		  }

		  else if(edge_capture == 0x08){
			  if(file_number < 2 )
				  file_number = 0;
			  else
				  file_number -= 2;

			  init_music();
		  }
	  }
  }

  return 0;
}
