// Lab3.c
// Runs on LM4F120/TM4C123
// Real Time Operating System for Labs 2 and 3
// Lab2 Part 1: Lab3Testmain1 and Lab3Testmain2
// Lab2 Part 2: Lab3Testmain3, Lab3Testmain4, Lab3Testmain5, Lab3TestmainCS and Lab3Realmain
// Lab3: Lab3Testmain6, Lab3Testmain7, Lab3TestmainCS and Lab3Realmain (with SW2)

// Jonathan W. Valvano 1/29/20, valvano@mail.utexas.edu
// EE445M/EE380L.12
// You may use, edit, run or distribute this file 
// You are free to change the syntax/organization of this file

// LED outputs to logic analyzer for use by OS profile 
// PF1 is preemptive thread switch
// PF2 is first periodic task (DAS samples PE3)
// PF3 is second periodic task (PID)
// PC4 is PF4 button touch (SW1 task)

// IR distance sensors
// J5/A3/PE3 analog channel 0  <- connect an IR distance sensor to J5 to get a realistic analog signal on PE3
// J6/A2/PE2 analog channel 1  <- connect an IR distance sensor to J6 to get a realistic analog signal on PE2
// J7/A1/PE1 analog channel 2
// J8/A0/PE0 analog channel 3  

// Button inputs
// PF0 is SW2 task (Lab3)
// PF4 is SW1 button input

// Analog inputs
// PE3 Ain0 sampled at 2kHz, sequencer 3, by DAS, using software start in ISR
// PE2 Ain1 sampled at 250Hz, sequencer 0, by Producer, timer tigger

#include <stdint.h>
#include "../inc/tm4c123gh6pm.h"
#include "../inc/CortexM.h"
#include "../inc/LaunchPad.h"
#include "../inc/PLL.h"
#include "../inc/LPF.h"
#include "../RTOS_Labs_common/UART0int.h"
#include "../RTOS_Labs_common/ADC.h"
#include "../inc/ADCT0ATrigger.h"
#include "../inc/IRDistance.h"
#include "../RTOS_Labs_common/OS.h"
#include "../RTOS_Labs_common/Interpreter.h"
#include "../RTOS_Labs_common/ST7735.h"


extern void jitterCalc(uint32_t num, uint32_t period);

//*********Prototype for FFT in cr4_fft_64_stm32.s, STMicroelectronics
void cr4_fft_64_stm32(void *pssOUT, void *pssIN, unsigned short Nbin);
//*********Prototype for PID in PID_stm32.s, STMicroelectronics
short PID_stm32(short Error, short *Coeff);

uint32_t Lab3NumCreated;   // number of foreground threads created
uint32_t Lab3IdleLab3Count;    // CPU idle counter
uint32_t PIDWork;      // current number of PID calculations finished
uint32_t FilterWork;   // number of digital filter calculations finished
uint32_t NumSamples;   // incremented every ADC sample, in Producer
#define FS 400              // producer/consumer sampling
#define RUNLENGTH (20*FS)   // display results and quit when NumSamples==RUNLENGTH
// 20-sec finite time experiment duration 

#define PERIOD1 TIME_500US   // DAS 2kHz sampling period in system time units
#define PERIOD2 TIME_1MS     // PID period in system time units
int32_t x[64],y[64];           // input and output arrays for FFT

// Lab3Idle reference count for 10ms of completely idle CPU
// This should really be calibrated in a 10ms delay loop in OS_Init()
uint32_t Lab3IdleLab3CountRef = 30769;
uint32_t CPUUtil;       // calculated CPU utilization (in 0.01%)

//---------------------User debugging-----------------------
uint32_t DataLost;     // data sent by Producer, but not received by Consumer
extern int32_t MaxJitter;             // largest time jitter between interrupts in usec
extern uint32_t const JitterSize;
extern uint32_t JitterHistogram1[];

extern int32_t MaxJitter2;             // largest time jitter between interrupts in usec
extern uint32_t const JitterSize2;
extern uint32_t JitterHistogram2[];

extern uint32_t Period1;
extern uint32_t Period2;

#define PD0  (*((volatile uint32_t *)0x40007004))
#define PD1  (*((volatile uint32_t *)0x40007008))
#define PD2  (*((volatile uint32_t *)0x40007010))
#define PD3  (*((volatile uint32_t *)0x40007020))

void Lab3PortD_Init(void){ 
  SYSCTL_RCGCGPIO_R |= 0x08;       // activate port D
  while((SYSCTL_RCGCGPIO_R&0x08)==0){};      
  GPIO_PORTD_DIR_R |= 0x0F;        // make PD3-0 output heartbeats
  GPIO_PORTD_AFSEL_R &= ~0x0F;     // disable alt funct on PD3-0
  GPIO_PORTD_DEN_R |= 0x0F;        // enable digital I/O on PD3-0
  GPIO_PORTD_PCTL_R = ~0x0000FFFF;
  GPIO_PORTD_AMSEL_R &= ~0x0F;;    // disable analog functionality on PD
}

//------------------Task 1--------------------------------
// 2 kHz sampling ADC channel 0, using software start trigger
// background thread executed at 2 kHz
// 60-Hz notch high-Q, IIR filter, assuming fs=2000 Hz
// y(n) = (256x(n) -503x(n-1) + 256x(n-2) + 498y(n-1)-251y(n-2))/256 (2k sampling)

//******** DAS *************** 
// background thread, calculates 60Hz notch filter
// runs 2000 times/sec
// samples analog channel 0, PE3,
// inputs:  none
// outputs: none
uint32_t DASoutput;
void DAS(void){ 
	//jitterCalc(0, PERIOD1);
  uint32_t input;  
  if(NumSamples < RUNLENGTH){   // finite time run
    PD0 ^= 0x01;
    input = ADC_In();           // channel set when calling ADC_Init
    PD0 ^= 0x01;
    DASoutput = Filter(input);
    FilterWork++;        // calculation finished
    PD0 ^= 0x01;
  }
}

//--------------end of Task 1-----------------------------

//------------------Task 2--------------------------------
// background thread executes with SW1 button
// one foreground task created with button push
// foreground treads run for 2 sec and die

// ***********ButtonWork*************
void ButtonWork(void){
  uint32_t myId = OS_Id(); 
  PD1 ^= 0x02;
  ST7735_Message(1,0,"Lab3NumCreated   =",Lab3NumCreated); 
  PD1 ^= 0x02;
  OS_Sleep(50);     // set this to sleep for 50msec
  ST7735_Message(1,1,"CPUUtil 0.01%=",CPUUtil);
  ST7735_Message(1,2,"DataLost     =",DataLost);
  ST7735_Message(1,3,"Jitter 0.1us =",MaxJitter);
  PD1 ^= 0x02;
  OS_Kill();  // done, OS does not return from a Kill
} 

//************SW1Push*************
// Called when SW1 Button pushed
// Adds another foreground task
// background threads execute once and return
void SW1Push(void){
  if(OS_MsTime() > 20){ // debounce
    if(OS_AddThread(&ButtonWork,100,2)){
      Lab3NumCreated++; 
    }
    OS_ClearMsTime();  // at least 20ms between touches
  }
}

//************SW2Push*************
// Called when SW2 Button pushed, Lab 3 only
// Adds another foreground task
// background threads execute once and return
void SW2Push(void){
  if(OS_MsTime() > 20){ // debounce
    if(OS_AddThread(&ButtonWork,100,2)){
      Lab3NumCreated++; 
    }
    OS_ClearMsTime();  // at least 20ms between touches
  }
}

//--------------end of Task 2-----------------------------

//------------------Task 3--------------------------------
// hardware timer-triggered ADC sampling at 400Hz
// Producer runs as part of ADC ISR
// Producer uses fifo to transmit 400 samples/sec to Consumer
// every 64 samples, Consumer calculates FFT
// every 2.5ms*64 = 160 ms (6.25 Hz), consumer sends data to Display via mailbox
// Display thread updates LCD with measurement

//******** Producer *************** 
// The Producer in this lab will be called from an ADC ISR
// A timer runs at 400Hz, started through the provided ADCT0ATrigger.c driver
// The timer triggers the ADC, creating the 400Hz sampling
// The ADC ISR runs when ADC data is ready
// The ADC ISR calls this function with a 12-bit sample 
// sends data to the consumer, runs periodically at 400Hz
// inputs:  none
// outputs: none
void Producer(uint32_t data){  
  if(NumSamples < RUNLENGTH){   // finite time run
    NumSamples++;               // number of samples
    if(OS_Fifo_Put(data) == 0){ // send to consumer
      DataLost++;
    } 
  } 
}

//******** Consumer *************** 
// foreground thread, accepts data from producer
// calculates FFT, sends DC component to Display
// inputs:  none
// outputs: none
void Display(void); 
void Consumer(void){ 
  uint32_t data,DCcomponent;   // 12-bit raw ADC sample, 0 to 4095
  uint32_t t;                  // time in 2.5 ms
  ADC0_InitTimer0ATriggerSeq0(1, FS, &Producer); // start ADC sampling, channel 1, PE2, 400 Hz
  Lab3NumCreated += OS_AddThread(&Display,128,0); 
  while(NumSamples < RUNLENGTH) { 
    PD2 = 0x04;
    for(t = 0; t < 64; t++){   // collect 64 ADC samples
      data = OS_Fifo_Get();    // get from producer
      x[t] = data;             // real part is 0 to 4095, imaginary part is 0
    }
    PD2 = 0x00;
    cr4_fft_64_stm32(y,x,64);  // complex FFT of last 64 ADC values
    DCcomponent = y[0]&0xFFFF; // Real part at frequency 0, imaginary part should be zero
    OS_MailBox_Send(DCcomponent); // called every 2.5ms*64 = 160ms
  }
  OS_Kill();  // done
}

//******** Display *************** 
// foreground thread, accepts data from consumer
// displays calculated results on the LCD
// inputs:  none                            
// outputs: none
void Display(void){ 
  uint32_t data,voltage,distance;
  uint32_t myId = OS_Id();
  ST7735_Message(0,1,"Run length = ",(RUNLENGTH)/FS); // top half used for Display
  while(NumSamples < RUNLENGTH) { 
    data = OS_MailBox_Recv();
    voltage = 3000*data/4095;   // calibrate your device so voltage is in mV
    distance = IRDistance_Convert(data,1); // you will calibrate this in Lab 6
    PD3 = 0x08;
    ST7735_Message(0,2,"v(mV) =",voltage);  
    ST7735_Message(0,3,"d(mm) =",distance);  
    PD3 = 0x00;
  } 
  OS_Kill();  // done
} 

//--------------end of Task 3-----------------------------

//------------------Task 4--------------------------------
// background thread that executes a digital controller 

//******** PID *************** 
// background thread, runs a PID controller
// runs every 1ms
// inputs:  none
// outputs: none
short Lab3IntTerm;     // accumulated error, RPM-sec
short Lab3PrevError;   // previous error, RPM
short Coeff[3] = { // PID coefficients
  384,  // 1.5 = 384/256 proportional coefficient
  128,  // 0.5 = 128/256 integral coefficient
  64    // 0.25 = 64/256 derivative coefficient*
};    
short Actuator;
void PID(void){ 
  static short err = -1000;  // speed error, range -100 to 100 RPM
  Actuator = PID_stm32(err,Coeff)/256;
  err++; 
  if(err > 1000) err = -1000; // made-up data
  PIDWork++;
}

//--------------end of Task 4-----------------------------

//------------------Task 5--------------------------------
// UART background ISR performs serial input/output
// Two software fifos are used to pass I/O data to foreground
// The interpreter runs as a foreground thread
// The UART driver should call OS_Wait(&RxDataAvailable) when foreground tries to receive
// The UART ISR should call OS_Signal(&RxDataAvailable) when it receives data from Rx
// Similarly, the transmit channel waits on a semaphore in the foreground
// and the UART ISR signals this semaphore (TxRoomLeft) when getting data from fifo

//******** Interpreter *************** 
// Modify your intepreter from Lab 1, adding commands to help debug 
// Interpreter is a foreground thread, accepts input from serial port, outputs to serial port
// inputs:  none
// outputs: none
void Interpreter(void);    // just a prototype, link to your interpreter
// add the following commands, leave other commands, if they make sense
// 1) print performance measures 
//    time-jitter, number of data points lost, number of calculations performed
//    i.e., NumSamples, Lab3NumCreated, MaxJitter, DataLost, FilterWork, PIDwork
      
// 2) print debugging parameters 
//    i.e., x[], y[] 

//--------------end of Task 5-----------------------------

//------------------Task 6--------------------------------
// foreground idle thread that always runs without waiting or sleeping

//******** Lab3Idle Task *************** 
// foreground thread, runs when nothing else does
// never blocks, never sleeps, never dies
// measures CPU idle time, i.e. CPU utilization
// inputs:  none
// outputs: none
void Lab3Idle(void){
  // measure idle time only for the first 20s for this lab	
  while(NumSamples < RUNLENGTH){
    Lab3IdleLab3Count++;  // measure of CPU idle time
  }
  
  // compute CPU utilization (in 0.01%)
  CPUUtil = 10000 - (5*Lab3IdleLab3Count)/Lab3IdleLab3CountRef;
  
  while(1) {
    // if you do not wish to measure CPU utilization using this idle task
    // you can execute WaitForInterrupt to put processor to sleep
    WaitForInterrupt();
  }
}

//--------------end of Task 6-----------------------------

//*******************final user main DEMONTRATE THIS TO TA**********
int Lab3Realmain(void){ // Lab3Realmain
  OS_Init();        // initialize, disable interrupts
  Lab3PortD_Init();     // debugging profile
  MaxJitter = 0;    // in 1us units
  DataLost = 0;     // lost data between producer and consumer
  Lab3IdleLab3Count = 0;
  CPUUtil = 0;
  NumSamples = 0;
  FilterWork = 0;
  PIDWork = 0;
	
  // initialize communication channels
  OS_MailBox_Init();
  OS_Fifo_Init(64);    // ***note*** 4 is not big enough*****

  // hardware init
  ADC_Init(0);  // sequencer 3, channel 0, PE3, sampling in DAS() 

  // attach background tasks
  OS_AddSW1Task(&SW1Push,2);
  OS_AddSW2Task(&SW2Push,2);  // added in Lab 3
  OS_AddPeriodicThread(&DAS,PERIOD1,1); // 2 kHz real time sampling of PE3
  OS_AddPeriodicThread(&PID,PERIOD2,2); // Lab 3 PID, lowest priority

  // create initial foreground threads
  Lab3NumCreated = 0;
  Lab3NumCreated += OS_AddThread(&Consumer,128,1); 
  //Lab3NumCreated += OS_AddThread(&Interpreter,128,2); 
  Lab3NumCreated += OS_AddThread(&Lab3Idle,128,5);  // Lab 3, at lowest priority 
 
  OS_Launch(10*TIME_1MS); // doesn't return, interrupts enabled in here
  return 0;            // this never executes
}

//+++++++++++++++++++++++++DEBUGGING CODE++++++++++++++++++++++++
// ONCE YOUR RTOS WORKS YOU CAN COMMENT OUT THE REMAINING CODE
// 

uint32_t Lab3Count1;   // number of times thread1 loops
uint32_t Lab3Count2;   // number of times thread2 loops
uint32_t Lab3Count3;   // number of times thread3 loops
uint32_t Lab3Count4;   // number of times thread4 loops
uint32_t Lab3Count5;   // number of times thread5 loops

//*******************Fourth TEST**********
// Once the third test runs, test this (Lab 2 part 2 and Lab 3)
// no UART1 interrupts
// SYSTICK interrupts, with or without period established by OS_Launch
// Timer interrupts, with or without period established by OS_AddPeriodicThread
// PortF GPIO interrupts, active low
// no ADC serial port or LCD output
// tests priorities and blocking semaphores, tests Sleep and Kill
Sema4Type Readyd;        // set in background
int Lost;
void BackgroundLab3Thread1d(void){   // called at 1000 Hz
  Lab3Count1++;
  OS_Signal(&Readyd);
}
void Lab3Thread5d(void){
  for(;;){
    OS_Wait(&Readyd);
    Lab3Count5++;   // Lab3Count2 + Lab3Count5 should equal Lab3Count1 
    Lost = Lab3Count1-Lab3Count5-Lab3Count2;
  }
}
void Lab3Thread2d(void){
  OS_InitSemaphore(&Readyd,0);
  Lab3Count1 = 0;    // number of times signal is called      
  Lab3Count2 = 0;    
  Lab3Count5 = 0;    // Lab3Count2 + Lab3Count5 should equal Lab3Count1  
  Lab3NumCreated += OS_AddThread(&Lab3Thread5d,128,1); 
  OS_AddPeriodicThread(&BackgroundLab3Thread1d,TIME_1MS,0); 
  for(;;){
    OS_Wait(&Readyd);
    Lab3Count2++;   // Lab3Count2 + Lab3Count5 should equal Lab3Count1
    Lost = Lab3Count1-Lab3Count5-Lab3Count2;
  }
}
void Lab3Thread3d(void){
  Lab3Count3 = 0;          
  for(;;){
    Lab3Count3++;
  }
}
void Lab3Thread4d(void){ int i;
  for(i=0;i<64;i++){
    Lab3Count4++;
    OS_Sleep(10);
  }
  OS_Kill();
  Lab3Count4 = 0;
}
void BackgroundLab3Thread5d(void){   // called when Select button pushed
  Lab3NumCreated += OS_AddThread(&Lab3Thread4d,128,1); 
}
      
int Lab3Testmain4(void){   // Lab3Testmain4
  Lab3Count4 = 0;          
  OS_Init();           // initialize, disable interrupts
  // Lab3Count2 + Lab3Count5 should equal Lab3Count1
  // With priorities, Lab3Count5 should be zero 
  // Lab3Count4 increases by 64 every time select is pressed
  Lab3NumCreated = 0 ;
  OS_AddSW1Task(&BackgroundLab3Thread5d,2);
  Lab3NumCreated += OS_AddThread(&Lab3Thread2d,128,0); // Lab 3 highest priority 
  Lab3NumCreated += OS_AddThread(&Lab3Thread3d,128,1); 
  Lab3NumCreated += OS_AddThread(&Lab3Thread4d,128,1); 
  OS_Launch(TIME_2MS); // doesn't return, interrupts enabled in here
  return 0;            // this never executes
}

//*******************Fith TEST**********
// Once the fourth test runs, run this example (Lab 2 part 2 and Lab 3)
// no UART interrupts
// SYSTICK interrupts, with or without period established by OS_Launch
// Timer interrupts, with or without period established by OS_AddPeriodicThread
// Select switch interrupts, active low
// no ADC serial port or LCD output
// tests the blocking semaphores, tests Sleep and Kill
// uses priorities to test proper blocking of sempahore waits
Sema4Type Readye;        // set in background
void BackgroundLab3Thread1e(void){   // called at 1000 Hz
static int i=0;
  i++;
  if(i==50){
    i = 0;         //every 50 ms
    Lab3Count1++;
    OS_bSignal(&Readye);
  }
}
void Lab3Thread2e(void){
  OS_InitSemaphore(&Readye,0);
  Lab3Count1 = 0;          
  Lab3Count2 = 0;          
  for(;;){
    OS_bWait(&Readye);
		//PF1 ^= 0x02;
    Lab3Count2++;     // Lab3Count2 should be equal to Lab3Count1
  }
}
void Lab3Thread3e(void){
  Lab3Count3 = 0;          
  for(;;){
		//PF2 ^= 0x04;
    Lab3Count3++;     // Lab3Count3 should be large
  }
}
void Lab3Thread4e(void){ int i;
  for(i=0;i<640;i++){
		//PF3 ^= 0x08;
    Lab3Count4++;     // Lab3Count4 should increase on button press
    OS_Sleep(1);
  }
  OS_Kill();
}
void BackgroundLab3Thread5e(void){   // called when Select button pushed
  Lab3NumCreated += OS_AddThread(&Lab3Thread4e,128,1); 
}

int Lab3Testmain5(void){   // Lab3Testmain5
  Lab3Count4 = 0;          
  OS_Init();           // initialize, disable interrupts
  // Lab3Count1 should exactly equal Lab3Count2
  // Lab3Count3 should be very large
  // Lab3Count4 increases by 640 every time select is pressed
  Lab3NumCreated = 0 ;
  OS_AddPeriodicThread(&BackgroundLab3Thread1e,PERIOD1,0); 
  OS_AddSW1Task(&BackgroundLab3Thread5e,2);
  Lab3NumCreated += OS_AddThread(&Lab3Thread2e,128,0); // Lab 3 set to highest priority
  Lab3NumCreated += OS_AddThread(&Lab3Thread3e,128,2); 
  Lab3NumCreated += OS_AddThread(&Lab3Thread4e,128,2); 
  OS_Launch(TIME_2MS); // doesn't return, interrupts enabled in here
  return 0;            // this never executes
}

//******************* Lab 3 Procedure 2**********
// Modify this so it runs with your RTOS (i.e., fix the time units to match your OS)
// run this with 
// UART0, 115200 baud rate, used to output results 
// SYSTICK interrupts, period established by OS_Launch
// first timer interrupts, period established by first call to OS_AddPeriodicThread
// second timer interrupts, period established by second call to OS_AddPeriodicThread
// SW1 no interrupts
// SW2 no interrupts
uint32_t Lab3CountA;   // number of times Task A called
uint32_t Lab3CountB;   // number of times Task B called
uint32_t Lab3Count1;   // number of times thread1 loops

// simple time delay, simulates user program doing real work
// Input: amount of work in 100ns units (free free to change units)
void PseudoWork(uint32_t work){
uint32_t startTime;
  startTime = OS_Time();    // time in 100ns units
  while(OS_TimeDifference(startTime,OS_Time()) <= work){} 
}
void Lab3Thread6(void){  // foreground thread
  Lab3Count1 = 0;          
  for(;;){
    Lab3Count1++; 
    PD0 ^= 0x01;        // debugging toggle bit 0  
  }
}
extern void Jitter(); // prints jitter information (write this)
extern void jitterCalc(uint32_t num, uint32_t period);
extern void jitterCalc2(uint32_t num, uint32_t period);
extern uint32_t dump[300];
void Lab3Thread7(void){  // foreground thread
  UART_OutString("\n\rEE345M/EE380L, Lab 3 Procedure 2\n\r");
  OS_Sleep(5000);   // 10 seconds        
  //Jitter(MaxJitter, JitterSize, JitterHistogram1, Period1, 1);  // print jitter information
  //Jitter(MaxJitter2, JitterSize2, JitterHistogram2, Period2, 2);  // print jitter of second thread
	Jitter();
	/*for(int i = 240; i < 260; i++){
		UART_OutUDec(dump[i]);
		UART_OutString(" ");
	} */
  UART_OutString("\n\r\n\r");
  OS_Kill();
}
#define workA 500       // {5,50,500 us} work in Task A
#define counts1us 1    // number of OS_Time counts per 1us
void TaskA(void){       // called every {1000, 2990us} in background
	//jitterCalc(0, 10*TIME_1MS);
  PD1 = 0x02;      // debugging profile  
  Lab3CountA++;
  PseudoWork(workA*counts1us); //  do work (100ns time resolution)
  PD1 = 0x00;      // debugging profile  
}
#define workB 250       // 250 us work in Task B
void TaskB(void){       // called every pB in background
	//jitterCalc(1, 20*TIME_1MS);							//!!!!!! bad jitter time because of UART and/or LCD
  PD2 = 0x04;      // debugging profile  
  Lab3CountB++;
  PseudoWork(workB*counts1us); //  do work (100ns time resolution)
  PD2 = 0x00;      // debugging profile  
}

int Lab3Testmain6(void){       // Lab3Testmain6 Lab 3
  Lab3PortD_Init();
  OS_Init();           // initialize, disable interrupts
  Lab3NumCreated = 0 ;
  Lab3NumCreated += OS_AddThread(&Lab3Thread7,128,1); 
  Lab3NumCreated += OS_AddThread(&Lab3Thread6,128,2); 
	OS_AddPeriodicThread(&TaskA,10*TIME_1MS,0);           // 1 ms, higher priority
	OS_AddPeriodicThread(&TaskB,20*TIME_1MS,1);         // 2 ms, lower priority
  
  
 
  OS_Launch(TIME_2MS); // 2ms, doesn't return, interrupts enabled in here
  return 0;             // this never executes
}

//******************* Lab 3 Procedure 4**********
// Modify this so it runs with your RTOS used to test blocking semaphores
// run this with 
// UART0, 115200 baud rate,  used to output results 
// SYSTICK interrupts, period established by OS_Launch
// first timer interrupts, period established by first call to OS_AddPeriodicThread
// second timer interrupts, period established by second call to OS_AddPeriodicThread
// SW1 no interrupts, 
// SW2 no interrupts
Sema4Type s;            // test of this counting semaphore
uint32_t SignalLab3Count1;   // number of times s is signaled
uint32_t SignalLab3Count2;   // number of times s is signaled
uint32_t SignalLab3Count3;   // number of times s is signaled
uint32_t WaitLab3Count1;     // number of times s is successfully waited on
uint32_t WaitLab3Count2;     // number of times s is successfully waited on
uint32_t WaitLab3Count3;     // number of times s is successfully waited on
#define MAXCOUNT 20000
void OutputLab3Thread(void){  // foreground thread
  UART_OutString("\n\rEE445M/EE380L, Lab 3 Procedure 4\n\r");
  while(SignalLab3Count1+SignalLab3Count2+SignalLab3Count3<3*MAXCOUNT){
    OS_Sleep(1000);   // 1 second
    UART_OutString(".");
  }  
	//long sr = StartCritical();
  UART_OutString(" done\n\r");
  UART_OutString("Signalled="); UART_OutUDec(SignalLab3Count1+SignalLab3Count2+SignalLab3Count3);
	UART_OutString(" Signal1="); UART_OutUDec(SignalLab3Count1);
	UART_OutString(" Signal2="); UART_OutUDec(SignalLab3Count2);
	UART_OutString(" Signal3="); UART_OutUDec(SignalLab3Count3);
  UART_OutString(", Waited="); UART_OutUDec(WaitLab3Count1+WaitLab3Count2+WaitLab3Count3);
  UART_OutString("\n\r");
	//EndCritical(sr);
	OS_Kill();
}
void Wait1(void){  // foreground thread
  for(;;){
    OS_Wait(&s);    // three threads waiting
    WaitLab3Count1++; 
  }
}
void Wait2(void){  // foreground thread
  for(;;){
    OS_Wait(&s);    // three threads waiting
    WaitLab3Count2++; 
  }
}
void Wait3(void){   // foreground thread
  for(;;){
    OS_Wait(&s);    // three threads waiting
    WaitLab3Count3++; 
  }
}
void Signal1(void){      // called every 799us in background
  if(SignalLab3Count1<1*MAXCOUNT){
    OS_Signal(&s);
    SignalLab3Count1++;
  }else{
		UART_OutString("Signal1 done in ");
		UART_OutUDec(OS_MsTime());
		UART_OutString("ms \n\r");
		//OS_Kill();
		WTIMER1_CTL_R &= ~0x00000001;    // 1) disable WTIMER1A
	}
}
// edit this so it changes the periodic rate
void Signal2(void){       // called every 1111us in background
  if(SignalLab3Count2<1*MAXCOUNT){
    OS_Signal(&s);
    SignalLab3Count2++;
  }else{
		UART_OutString("Signal2 done in ");
		UART_OutUDec(OS_MsTime());
		UART_OutString("ms \n\r");
		//OS_Kill();
		WTIMER1_CTL_R &= ~0x00000100;    // 1) disable WTIMER1B
	}
}
void Signal3(void){       // foreground
  while(SignalLab3Count3<1*MAXCOUNT){
    OS_Signal(&s);
    SignalLab3Count3++;
  }
	UART_OutString("Signal3 done in ");
	UART_OutUDec(OS_MsTime());
	UART_OutString("ms \n\r");
  OS_Kill();
}

int32_t add(const int32_t n, const int32_t m){
static int32_t result;
  result = m+n;
  return result;
}
int Lab3Testmain7(void){      // Lab3Testmain7  Lab 3
  volatile uint32_t delay;
  OS_Init();           // initialize, disable interrupts
  delay = add(3,4);
  Lab3PortD_Init();
  SignalLab3Count1 = 0;   // number of times s is signaled
  SignalLab3Count2 = 0;   // number of times s is signaled
  SignalLab3Count3 = 0;   // number of times s is signaled
  WaitLab3Count1 = 0;     // number of times s is successfully waited on
  WaitLab3Count2 = 0;     // number of times s is successfully waited on
  WaitLab3Count3 = 0;    // number of times s is successfully waited on
  OS_InitSemaphore(&s,0);   // this is the test semaphore
  OS_AddPeriodicThread(&Signal1,(799*TIME_1MS)/1000,0);   // 0.799 ms, higher priority
  OS_AddPeriodicThread(&Signal2,(1111*TIME_1MS)/1000,1);  // 1.111 ms, lower priority
  Lab3NumCreated = 0 ;
  Lab3NumCreated += OS_AddThread(&OutputLab3Thread,128,2);   // results output thread
  Lab3NumCreated += OS_AddThread(&Signal3,128,2);   // signalling thread
  Lab3NumCreated += OS_AddThread(&Wait1,128,2);   // waiting thread
  Lab3NumCreated += OS_AddThread(&Wait2,128,2);   // waiting thread
  Lab3NumCreated += OS_AddThread(&Wait3,128,2);   // waiting thread
  Lab3NumCreated += OS_AddThread(&Lab3Thread6,128,5);      // idle thread to keep from crashing
 
  OS_Launch(TIME_1MS);  // 1ms, doesn't return, interrupts enabled in here
  return 0;             // this never executes
}

//*******************Measurement of context switch time**********
// Run this to measure the time it takes to perform a task switch
// UART0 not needed 
// SYSTICK interrupts, period established by OS_Launch
// first timer not needed
// second timer not needed
// SW1 not needed, 
// SW2 not needed
// logic analyzer on PF1 for systick interrupt (in your OS)
//                on PD0 to measure context switch time
void Lab3ThreadCS(void){       // only thread running
  while(1){
    PD0 ^= 0x01;      // debugging profile  
  }
}
int Lab3TestmainCS(void){       // Lab3TestmainCS
  Lab3PortD_Init();
  OS_Init();           // initialize, disable interrupts
  Lab3NumCreated = 0 ;
  Lab3NumCreated += OS_AddThread(&Lab3ThreadCS,128,0); 
  OS_Launch(TIME_1MS/10); // 100us, doesn't return, interrupts enabled in here
  return 0;             // this never executes
}

//*******************FIFO TEST**********
// FIFO test
// Lab3Count1 should exactly equal Lab3Count2
// Lab3Count3 should be very large
// Timer interrupts, with period established by OS_AddPeriodicThread
uint32_t OtherLab3Count1;
uint32_t Expected8; // last data read+1
uint32_t Error8;
void ConsumerLab3ThreadFIFO(void){        
  Lab3Count2 = 0;          
  for(;;){
    OtherLab3Count1 = OS_Fifo_Get();
    if(OtherLab3Count1 != Expected8){
      Error8++;
    }
    Expected8 = OtherLab3Count1+1; // should be sequential
    Lab3Count2++;     
  }
}
void FillerLab3ThreadFIFO(void){
  Lab3Count3 = 0;          
  for(;;){
    Lab3Count3++;
  }
}
void BackgroundLab3ThreadFIFOProducer(void){   // called periodically
  if(OS_Fifo_Put(Lab3Count1) == 0){ // send to consumer
    DataLost++;
  }
  Lab3Count1++;
}

int Lab3TestmainFIFO(void){   // Lab3TestmainFIFO
  Lab3Count1 = 0;     DataLost = 0;  
  Expected8 = 0;  Error8 = 0;
  OS_Init();           // initialize, disable interrupts
  Lab3NumCreated = 0 ;
  OS_AddPeriodicThread(&BackgroundLab3ThreadFIFOProducer,PERIOD1,0); 
  OS_Fifo_Init(16);
  Lab3NumCreated += OS_AddThread(&ConsumerLab3ThreadFIFO,128,2); 
  Lab3NumCreated += OS_AddThread(&FillerLab3ThreadFIFO,128,3); 
  OS_Launch(TIME_2MS); // doesn't return, interrupts enabled in here
  return 0;            // this never executes
}

