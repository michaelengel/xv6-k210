//
// low-level driver routines for 16550a UART.
//


#include "include/types.h"
#include "include/param.h"
#include "include/memlayout.h"
#include "include/riscv.h"
#include "include/spinlock.h"
#include "include/proc.h"
#include "include/intr.h"
#include "include/sbi.h"

void consoleintr(int c);

// the UART control registers are memory-mapped
// at address UART0. this macro returns the
// address of one of the registers.
#define Reg(reg) ((volatile unsigned char *)(UART_V + 4 * reg))
#define ReadReg(reg) (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))

// the UART control registers.
// some have different meanings for
// read vs write.
// see http://byterunner.com/16550.html
#define RHR 0                 // receive holding register (for input bytes)
#define THR 0                 // transmit holding register (for output bytes)
#define IER 1                 // interrupt enable register
#define IER_TX_ENABLE (1<<0)
#define IER_RX_ENABLE (1<<1)
#define FCR 2                 // FIFO control register
#define IIR 2

#define UART_IIR_NOINT    0x01    /* no interrupt pending */
#define UART_IIR_IMA      0x06    /* interrupt identity:  */
#define UART_IIR_LSI      0x06    /*  - rx line status    */
#define UART_IIR_RDA      0x04    /*  - rx data recv'd    */
#define UART_IIR_THR      0x02    /*  - tx reg. empty     */
#define UART_IIR_MSI      0x00    /*  - MODEM status      */
#define UART_IIR_BSY      0x07    /*  - busy detect (DW) */

#define FCR_FIFO_ENABLE (1<<0)
#define FCR_FIFO_CLEAR (3<<1) // clear the content of the two FIFOs

#define LCR 3                 // line control register
#define LCR_EIGHT_BITS (3<<0)
#define LCR_BAUD_LATCH (1<<7) // special mode to set baud rate

#define LSR 5                 // line status register
#define LSR_RX_READY (1<<0)   // input is waiting to be read from RHR
#define LSR_TX_IDLE (1<<5)    // THR can accept another character to send
#define LSR_TX_EMPTY (1<<6)
#define USR 0x1f

// the transmit output buffer.
struct spinlock uart_tx_lock;
#define UART_TX_BUF_SIZE 32
char uart_tx_buf[UART_TX_BUF_SIZE];
int uart_tx_w; // write next to uart_tx_buf[uart_tx_w++]
int uart_tx_r; // read next from uart_tx_buf[uar_tx_r++]

extern volatile int panicked; // from printf.c

void uart_start();

void
uart_entxi(void)
{
  // enable transmit and receive interrupts.
  if ((ReadReg(IER) & IER_TX_ENABLE) == 0)
    WriteReg(IER, IER_TX_ENABLE | IER_RX_ENABLE);
}

void
uart_init(void)
{
#if 0
  // disable interrupts.
  WriteReg(IER, 0x00);

  // special mode to set baud rate.
  WriteReg(LCR, LCR_BAUD_LATCH);

  // LSB for baud rate of 38.4K.
  WriteReg(0, 0x03);

  // MSB for baud rate of 38.4K.
  WriteReg(1, 0x00);
#endif

  // leave set-baud mode,
  // and set word length to 8 bits, no parity.
  WriteReg(LCR, LCR_EIGHT_BITS);

  // reset and enable FIFOs.
  // WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR | 0x80);
  WriteReg(FCR, 0x00);

  // enable receive interrupts.
  WriteReg(IER, IER_RX_ENABLE);

  uart_tx_w = uart_tx_r = 0;
  initlock(&uart_tx_lock, "uart");
}

// add a character to the output buffer and tell the
// UART to start sending if it isn't already.
// blocks if the output buffer is full.
// because it may block, it can't be called
// from interrupts; it's only suitable for use
// by write().

void
uart_putchar(int c)
{
  acquire(&uart_tx_lock);
  
  if(panicked){
    for(;;)
      ;
  }

  while(1){
    if(((uart_tx_w + 1) % UART_TX_BUF_SIZE) == uart_tx_r){
      // buffer is full.
      // wait for uartstart() to open up space in the buffer.
      sleep(&uart_tx_r, &uart_tx_lock);
    } else {
      uart_tx_buf[uart_tx_w] = c;
      // uart_tx_w = (uart_tx_w + 1) % UART_TX_BUF_SIZE;
      uart_tx_w = (uart_tx_w + 1);
      uart_start();
      release(&uart_tx_lock);
      return;
    }
  }
}

// alternate version of uart_putchar() that doesn't 
// use interrupts, for use by kernel printf() and
// to echo characters. it spins waiting for the uart's
// output register to be empty.
void
uart_putc_sync(int c)
{
  push_off();

  if(panicked){
    for(;;)
      ;
  }

  // wait for Transmit Holding Empty to be set in LSR.
  while((ReadReg(LSR) & LSR_TX_IDLE) == 0)
    ;
  WriteReg(THR, c);

  pop_off();
}

// if the UART is idle, and a character is waiting
// in the transmit buffer, send it.
// caller must hold uart_tx_lock.
// called from both the top- and bottom-half.
void
uart_start()
{
  while(1){
    if(uart_tx_w == uart_tx_r){
      // transmit buffer is empty.
      return;
    }
    
    if((ReadReg(LSR) & LSR_TX_IDLE) == 0){
    // if((ReadReg(IIR) & 0x02) == 0){
      // the UART transmit holding register is full,
      // so we cannot give it another byte.
      // it will interrupt when it's ready for a new byte.
      return;
    }
    
    int c = uart_tx_buf[uart_tx_r];
    uart_tx_r = (uart_tx_r + 1) % UART_TX_BUF_SIZE;
    
    WriteReg(THR, c);

    // maybe uart_putchar() is waiting for space in the buffer.
    wakeup(&uart_tx_r);
  }
}

// read one input character from the UART.
// return -1 if none is waiting.
int
uart_getchar(void)
{
  if(ReadReg(LSR) & 0x01){
    // input data is ready.
    return ReadReg(RHR);
  } else {
    return -1;
  }
}

// handle a uart interrupt, raised because input has
// arrived, or the uart is ready for more output, or
// both. called from trap.c.
void
uart_intr(void)
{
  // fucking DW crap quirks...
  uint32 v1 = ReadReg(IIR);
  if ((v1 & 0x3f) == 0x0c) { // UART_IIR_RX_TIMEOUT
    (void)uart_getchar();
  }

  // read and process incoming characters.
  uint32 val = ReadReg(LSR);
  if (val & LSR_RX_READY) { // RX interrupt
    while(1){
      int c = uart_getchar();
      if(c == -1)
        break;
      consoleintr(c);
    }
  }

  if (val & LSR_TX_IDLE) { // TX interrupt
    // send buffered characters.
    acquire(&uart_tx_lock);
    uart_start();
    release(&uart_tx_lock);
  }

  if(ReadReg(IIR) & UART_IIR_BSY) {
    ReadReg(USR);
  } 
}
