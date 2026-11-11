/* RFID reader (EM18) : UART0  P0.2 (TXD0), P0.3 (RXD0)   9600 baud
 * GSM module (SIM900): UART2  P0.10 (TXD2), P0.11 (RXD2) 9600 baud
 * LCD : I2C0   P0.27 (SDA0), P0.28 (SCL0) needs pull-ups
 * Door lock / LED    : P2.0 (active high)
 */

#include <LPC17xx.h>
#include <string.h>

#define GSM_UART      LPC_UART2
#define RFID_UART     LPC_UART0
#define LCD_ADDR      0x4E               /* 0x27 << 1 (try 0x3F << 1 = 0x7E) */
#define ALERT_NUMBER  "+916362503131"    /* put the real number here */
#define ID_LEN        10                 /* EM18: 10 ID chars + 2 checksum chars */
#define FRAME_LEN     12

/* Replace with the IDs of your own cards */
static const char *authorized[] = { "0123456789", "9876543210" };
#define NUM_AUTH (sizeof(authorized) / sizeof(authorized[0]))

/* ---------- simple delay (approximate) ---------- */
static void delay_ms(uint32_t ms)
{
    volatile uint32_t n = ms * (SystemCoreClock / 10000);
    while (n--);
}

/* ---------- UART ---------- */
static void uart_init(LPC_UART_TypeDef *u, uint32_t baud)
{
    uint32_t dl = (SystemCoreClock / 4) / (16 * baud);  /* PCLK = CCLK/4 */
    u->LCR = 0x83;              /* 8 bits, 1 stop, no parity, DLAB = 1 */
    u->DLM = (dl >> 8) & 0xFF;
    u->DLL = dl & 0xFF;
    u->LCR = 0x03;              /* DLAB = 0 */
    u->FCR = 0x07;              /* enable and clear FIFOs */
}

static void uart_putc(LPC_UART_TypeDef *u, char c)
{
    while (!(u->LSR & 0x20));   /* wait until THR is empty */
    u->THR = c;
}

static void uart_puts(LPC_UART_TypeDef *u, const char *s)
{
    while (*s) uart_putc(u, *s++);
}

static char uart_getc(LPC_UART_TypeDef *u)
{
    while (!(u->LSR & 0x01));   /* wait until data is received */
    return u->RBR;
}

static void uart_flush_rx(LPC_UART_TypeDef *u)
{
    u->FCR = 0x03;              /* keep FIFOs enabled, reset the RX FIFO */
}

/* ---------- I2C0 (polling, no status checks, to keep it short) ---------- */
#define I2C_AA   0x04
#define I2C_SI   0x08
#define I2C_STO  0x10
#define I2C_STA  0x20
#define I2C_EN   0x40

static void i2c_init(void)
{
    uint32_t half = (SystemCoreClock / 4) / (2 * 100000);   /* 100 kHz */
    LPC_I2C0->I2SCLH = half;
    LPC_I2C0->I2SCLL = half;
    LPC_I2C0->I2CONCLR = I2C_AA | I2C_SI | I2C_STA | I2C_EN;
    LPC_I2C0->I2CONSET = I2C_EN;
}

static void i2c_wait(void)  { while (!(LPC_I2C0->I2CONSET & I2C_SI)); }

static void i2c_start(void)
{
    LPC_I2C0->I2CONSET = I2C_STA;
    i2c_wait();
}

static void i2c_write(uint8_t b)
{
    LPC_I2C0->I2DAT = b;
    LPC_I2C0->I2CONCLR = I2C_STA | I2C_SI;
    i2c_wait();
}

static void i2c_stop(void)
{
    LPC_I2C0->I2CONSET = I2C_STO;
    LPC_I2C0->I2CONCLR = I2C_SI;
}

/* ---------- LCD through PCF8574 (4-bit mode) ----------
 * PCF8574 bits: P0 = RS, P1 = RW, P2 = E, P3 = backlight, P4..P7 = D4..D7 */
static void pcf_write(uint8_t v)
{
    i2c_start();
    i2c_write(LCD_ADDR);
    i2c_write(v);
    i2c_stop();
}

static void lcd_nibble(uint8_t nib, uint8_t rs)
{
    uint8_t d = (uint8_t)((nib << 4) | 0x08 | rs);   /* backlight on */
    pcf_write(d | 0x04);    /* E = 1 */
    pcf_write(d);           /* E = 0 */
}

static void lcd_cmd(uint8_t c)
{
    lcd_nibble(c >> 4, 0);
    lcd_nibble(c & 0x0F, 0);
    delay_ms(2);
}

static void lcd_data(uint8_t c)
{
    lcd_nibble(c >> 4, 1);
    lcd_nibble(c & 0x0F, 1);
}

static void lcd_init(void)
{
    delay_ms(50);
    lcd_nibble(0x03, 0); delay_ms(5);
    lcd_nibble(0x03, 0); delay_ms(5);
    lcd_nibble(0x03, 0); delay_ms(5);
    lcd_nibble(0x02, 0); delay_ms(5);   /* switch to 4-bit mode */
    lcd_cmd(0x28);                      /* 4-bit, 2 lines */
    lcd_cmd(0x0C);                      /* display on, cursor off */
    lcd_cmd(0x06);                      /* auto-increment cursor */
    lcd_cmd(0x01);                      /* clear */
}

static void lcd_print(uint8_t row, const char *s)
{
    lcd_cmd(0x80 | (row ? 0x40 : 0x00));
    while (*s) lcd_data((uint8_t)*s++);
}

static void lcd_clear(void) { lcd_cmd(0x01); delay_ms(2); }

/* ---------- GSM: send SMS with AT commands ---------- */
static void gsm_send_sms(const char *number, const char *msg)
{
    uart_puts(GSM_UART, "AT\r\n");            delay_ms(500);
    uart_puts(GSM_UART, "AT+CMGF=1\r\n");     delay_ms(500);
    uart_puts(GSM_UART, "AT+CMGS=\"");
    uart_puts(GSM_UART, number);
    uart_puts(GSM_UART, "\"\r\n");            delay_ms(1000);
    uart_puts(GSM_UART, msg);
    uart_putc(GSM_UART, 0x1A);                /* Ctrl+Z ends the message */
    delay_ms(3000);
}

/* ---------- authentication ---------- */
static int is_authorized(const char *id)
{
    unsigned i;
    for (i = 0; i < NUM_AUTH; i++)
        if (strncmp(id, authorized[i], ID_LEN) == 0)
            return 1;
    return 0;
}

int main(void)
{
    char id[FRAME_LEN + 1];
    int i;

    SystemInit();
    SystemCoreClockUpdate();

    /* pin functions: UART0 (P0.2/P0.3), UART2 (P0.10/P0.11), I2C0 (P0.27/P0.28) */
    LPC_PINCON->PINSEL0 &= ~((3u << 4) | (3u << 6) | (3u << 20) | (3u << 22));
    LPC_PINCON->PINSEL0 |=  ((1u << 4) | (1u << 6) | (1u << 20) | (1u << 22));
    LPC_PINCON->PINSEL1 &= ~((3u << 22) | (3u << 24));
    LPC_PINCON->PINSEL1 |=  ((1u << 22) | (1u << 24));

    LPC_SC->PCONP |= (1u << 24);            /* power UART2 (UART0, I2C0 are on) */

    LPC_GPIO2->FIODIR |= (1u << 0);         /* door output */
    LPC_GPIO2->FIOCLR  = (1u << 0);

    uart_init(RFID_UART, 9600);
    uart_init(GSM_UART, 9600);
    i2c_init();
    lcd_init();

    while (1)
    {
        lcd_clear();
        uart_flush_rx(RFID_UART);           /* drop stale bytes from earlier reads */
        lcd_print(0, "Scan your card");

        for (i = 0; i < FRAME_LEN; i++)     /* wait for a full frame from the reader */
            id[i] = uart_getc(RFID_UART);
        id[FRAME_LEN] = '\0';

        lcd_clear();
        if (is_authorized(id))
        {
            lcd_print(0, "Access granted");
            LPC_GPIO2->FIOSET = (1u << 0);  /* unlock */
            delay_ms(3000);
            LPC_GPIO2->FIOCLR = (1u << 0);  /* lock again */
        }
        else
        {
            lcd_print(0, "Access denied");
            lcd_print(1, "Alert sent");
            gsm_send_sms(ALERT_NUMBER, "Unauthorized door access attempt!");
        }
        delay_ms(1000);
    }
}