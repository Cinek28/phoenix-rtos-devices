/*
 * Phoenix-RTOS
 *
 * STM32L1 internal flash driver
 *
 * Copyright 2017, 2018 Phoenix Systems
 * Author: Aleksander Kaminski, Jakub Sejdak
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#include ARCH
#include <sys/threads.h>
#include <sys/pwman.h>
#include <unistd.h>

#include "flash.h"
#include "log.h"


#define BUFFER_SIZE         128
#define FLASH_MAX_READ      64

#define FLASH_AREA_EVENTS   0
#define FLASH_AREA_LOGS     1


#ifndef NDEBUG
static const char drvname[] = "flash: ";
#endif


enum { flash_pecr = 1, flash_pekeyr = 3, flash_prgkeyr, flash_oprkeyr, flash_sr, flash_obr };


enum { EVENTS = 0, LOGS };


typedef struct {
	u32 addr;
	int currIdx;
	u64 currId;
	int entrySize;
	int entryCount;
} __attribute__((packed)) logarea_t;


struct {
	volatile unsigned int *flash;
//	volatile unsigned int *iwdg;
	unsigned int port;
	lock_t lock;

	char buff[FLASH_MAX_READ];
	char page[FLASH_PAGE_SIZE];
} flash_common;


struct {
	logarea_t area[2];
} log_common;


static int _flash_wait(void)
{
	int i;

	for (i = 0; i < 10; ++i) {
		/* Check if flash is busy. */
		if ((*(flash_common.flash + flash_sr) & 0x1) == 0x1) {
			keepidle(1);
			usleep(10000);
			keepidle(0);
//			*flash_common.iwdg = 0xaaaa;
			continue;
		}

		/* Check if flash is write protected. */
		if (*(flash_common.flash + flash_sr) & (1 << 8))
			return 1;

		return (*(flash_common.flash + flash_sr) & 0x1e00);
	}

	return 1;
}


static inline void _flash_clearFlags(void)
{
	*(flash_common.flash + flash_sr) |= 0x3f02;
}


static inline int eeprom_isValidAdress(u32 addr)
{
	return (addr >= FLASH_EEPROM_1_ADDR && addr <= (FLASH_EEPROM_2_ADDR + FLASH_EEPROM_SIZE));
}


static inline void _eeprom_lock(void)
{
	*(flash_common.flash + flash_pecr) |= (1 << 0);
	dataBarier();
}


static void _eeprom_unlock(void)
{
	while (*(flash_common.flash + flash_pecr) & (1 << 0)) {
		*(flash_common.flash + flash_pekeyr) = 0x89abcdef;
		dataBarier();
		*(flash_common.flash + flash_pekeyr) = 0x02030405;
		dataBarier();
	}
}


int _eeprom_eraseByte(u32 addr)
{
	int err;

	_eeprom_unlock();
	_flash_clearFlags();

	if ((err = _flash_wait()) == 0)
		*(volatile u8 *) addr = 0x0;

	_eeprom_lock();

	return err;
}


static int _eeprom_writeByte(u32 addr, char value)
{
	int err;

	_eeprom_unlock();
	_flash_clearFlags();

	if ((err = _flash_wait()) == 0) {
		*(volatile u8 *) addr = value;
		err = _flash_wait();
	}

	_eeprom_lock();

	return err;
}


static size_t eeprom_readData(u32 offset, char *buff, size_t size)
{
	unsigned int i;

	mutexLock(flash_common.lock);
	for (i = 0; i < size; ++i)
		buff[i] = *((volatile u8 *) offset + i);
	mutexUnlock(flash_common.lock);

	return i;
}


static size_t eeprom_writeData(u32 offset, char *buff, size_t size)
{
	unsigned int i;

	for (i = 0; i < size; ++i) {
		mutexLock(flash_common.lock);
		_eeprom_eraseByte(offset + i);
		_eeprom_writeByte(offset + i, buff[i]);
		mutexUnlock(flash_common.lock);
	}

	return i;
}


static inline int ob_isValidAdress(u32 addr)
{
	/* Address must be even. */
	if (addr & 1)
		return 0;

	if (!flash_activeBank()) {
		if (addr >= FLASH_OB_1_ADDR && addr <= (FLASH_OB_1_ADDR + FLASH_OB_SIZE))
			return 1;
	}
	else {
		if (addr >= FLASH_OB_2_ADDR && addr <= (FLASH_OB_2_ADDR + FLASH_OB_SIZE))
			return 1;
	}

	return 0;
}


static inline void _ob_lock(void)
{
	*(flash_common.flash + flash_pecr) |= (1 << 2);
}


static void _ob_unlock(void)
{
	while (*(flash_common.flash + flash_pecr) & (1 << 2)) {
		_eeprom_unlock();

		*(flash_common.flash + flash_oprkeyr) = 0xfbead9c8;
		dataBarier();
		*(flash_common.flash + flash_oprkeyr) = 0x24252627;
		dataBarier();
	}
}


static int ob_writeByte(u32 addr, char value)
{
	int err;
	volatile u32 word;
	volatile u32 *wordAddr = (u32 *) (addr & ~((u32) 0x3));

	mutexLock(flash_common.lock);
	_ob_unlock();
	_flash_clearFlags();

	if ((err = _flash_wait()) == 0) {
		word = *wordAddr;
		if (addr & (1 << 0)) {
			word &= 0x00ff00ff;
			word |= ((u32) value) << 8;
			word |= ((u32) ~value) << 24;
		}
		else {
			word &= 0xff00ff00;
			word |= ((u32) value);
			word |= ((u32) ~value) << 16;
		}


		*wordAddr = word;
		err = _flash_wait();
	}

	_ob_lock();
	mutexUnlock(flash_common.lock);

	return err;
}


static size_t ob_readData(u32 offset, char *buff, size_t size)
{
	unsigned int i;

	for (i = 0; i < size; ++i)
		buff[i] = *((volatile u8 *) offset + i);

	return i;
}


static size_t ob_writeData(u32 offset, char *buff, size_t size)
{
	if (size != 1)
		return 0;

	ob_writeByte(offset, buff[0]);

	return 1;
}


static inline int program_isValidAddress(u32 addr)
{
	if (addr >= FLASH_PROGRAM_1_ADDR && addr <= (FLASH_PROGRAM_1_ADDR + FLASH_PROGRAM_SIZE))
		return 1;

	if (addr >= FLASH_PROGRAM_2_ADDR && addr <= (FLASH_PROGRAM_2_ADDR + FLASH_PROGRAM_SIZE))
		return 1;

	return 0;
}


static inline void _program_lock(void)
{
	*(flash_common.flash + flash_pecr) |= (1 << 1);
	dataBarier();
}


static void _program_unlock(void)
{

	while (*(flash_common.flash + flash_pecr) & (1 << 1)) {
		_eeprom_unlock();

		*(flash_common.flash + flash_prgkeyr) = 0x8c9daebf;
		dataBarier();
		*(flash_common.flash + flash_prgkeyr) = 0x13141516;
		dataBarier();
	}
}


static int program_erasePage(u32 addr)
{
	int err;

	mutexLock(flash_common.lock);
	_program_unlock();
	_flash_clearFlags();

	if ((err = _flash_wait()) == 0) {
		/* Set the ERASE and PROG bits. */
		*(flash_common.flash + flash_pecr) |= ((1 << 9) | (1 << 3));

		/* Erase page. */
		*(volatile u32 *) addr = 0x0;
		err = _flash_wait();

		/* Disable the ERASE and PROG bits. */
		*(flash_common.flash + flash_pecr) &= ~((1 << 9) | (1 << 3));
	}

	_program_lock();
	mutexUnlock(flash_common.lock);

	return err;
}


static int program_writeWord(u32 addr, u32 value)
{
	int err;
	volatile u32 *current = (volatile u32 *) addr;

	mutexLock(flash_common.lock);
	_program_unlock();
	_flash_clearFlags();

	if ((err = _flash_wait()) == 0)  {
		if (*current == value) {
			_program_lock();
			mutexUnlock(flash_common.lock);
			return 0;
		}

		*current = value;
		err = _flash_wait();
	}

	_program_lock();
	mutexUnlock(&flash_common.lock);

	return err;
}


static size_t program_readData(u32 offset, char *buff, size_t size)
{
	unsigned int i, j, n = 0;
	unsigned int prefixBytes = min(4 - (offset & 0x3), size);
	unsigned int suffixBytes = (size - prefixBytes) % 4;
	unsigned int middleBytes = size - prefixBytes - suffixBytes;
	volatile u32 *addr = (u32 *) (offset & ~((u32) 0x3));
	volatile u32 value;

	/* Read prefix word. */
	value = *addr++;
	for (i = 0; i < prefixBytes; ++i, ++n)
		buff[n] = value >> (i * 8);

	/* Read middle section. */
	for (i = 0; i < middleBytes; i += 4, n += 4) {
		value = *addr++;
		for (j = 0; j < 4; ++j)
			buff[n + j] = value >> (j * 8);
	}

	/* Read suffix word. */
	if (suffixBytes) {
		value = *addr;
		for (i = 0; i < suffixBytes; ++i, ++n)
			buff[n] = value >> (i * 8);
	}

	return n;
}


static size_t program_writeData(u32 offset, char *buff, size_t size)
{
	u32 word, pageAddr, addr = offset;
	size_t n = 0;
	int i, j, toSkip;

	while (n < size) {
		/* Read page into buffer and erase it. */
		pageAddr = addr & ~((u32) (FLASH_PAGE_SIZE - 1));
		program_readData(pageAddr, flash_common.page, FLASH_PAGE_SIZE);
		program_erasePage(pageAddr);

		/* Modify data in buffer. */
		toSkip = addr - pageAddr;
		for (i = toSkip; (i < FLASH_PAGE_SIZE) && n < size; ++i, ++n)
			flash_common.page[i] = buff[n];

		/* Write back page. */
		for (i = 0; i < FLASH_PAGE_SIZE; i += 4) {
			for (j = 0, word = 0; j < 4; ++j)
				word |= flash_common.page[i + j] << 8 * j;

			program_writeWord(pageAddr + i, word);
		}

		addr += FLASH_PAGE_SIZE;
	}

	return n;
}


size_t flash_readData(u32 offset, char *buff, size_t size)
{
	if (program_isValidAddress(offset))
		return program_readData(offset, buff, size);

	if (eeprom_isValidAdress(offset))
		return eeprom_readData(offset, buff, size);

	if (ob_isValidAdress(offset))
		return ob_readData(offset, buff, size);

	return 0;
}


size_t flash_writeData(u32 offset, char *buff, size_t size)
{
	if (program_isValidAddress(offset))
		return program_writeData(offset, buff, size);

	if (eeprom_isValidAdress(offset))
		return eeprom_writeData(offset, buff, size);

	if (ob_isValidAdress(offset))
		return ob_writeData(offset, buff, size);

	return 0;
}


void flash_bankBreak(void)
{
	spinlock_t spinlock;
	char buff[4] = { 0x11, 0x11, 0x11, 0x11 };
	u32 currentBankAddress = flash_activeBank() ? FLASH_PROGRAM_2_ADDR : FLASH_PROGRAM_1_ADDR;

	/* Set option bytes so boot bank will be choosen on startup based on bank first word */
	ob_writeByte(0x1ff80004, 0x78);

	hal_spinlockCreate(&spinlock, "bankBreak");
	hal_spinlockSet(&spinlock);

	flash_writeData(currentBankAddress, buff, sizeof(buff));

	/* force reload of option bytes */
	ob_unlock();
	*(flash_common.flash + flash_pecr) |= (1 << 18);
	ob_lock();

	hal_cpuRestart();

	hal_spinlockClear(&spinlock);
	hal_spinlockDestroy(&spinlock);
}


static int _flash_atomCopy(u32 dest, u32 src, size_t len)
{
	hal_cpuDisableInterrupts();

	flash_writeData(dest, (void *)src, len);

	hal_cpuRestart();

	/* Should never reach here */
	return 0;
}


static void flash_thread(void *arg)
{
	char msg[BUFFER_SIZE];
	msghdr_t hdr;
	msgdata_t *data;
	flashdevctl_t *devctl;
	flashevent_t event;
	flashlog_t log;
	flasheeprominfo_t info;
	int count, bank, res;

	eeprom_init();

	for (;;) {
		int size = proc_recv(flash_common.port, msg, sizeof(msg), &hdr);
		if (hdr.type == MSG_NOTIFY)
			continue;

		switch (hdr.op) {
			case MSG_READ:
				if (size != sizeof(msgdata_t)) {
					proc_respond(flash_common.port, EINVAL, NULL, 0);
					break;
				}

				data = (msgdata_t *) msg;
				size = min(FLASH_MAX_READ, data->size);

				count = flash_readData(data->offset, flash_common.buff, size);
				proc_respond(flash_common.port, EOK, flash_common.buff, count);
				break;

			case MSG_WRITE:
				if (size != sizeof(msgdata_t)) {
					proc_respond(flash_common.port, EINVAL, NULL, 0);
					break;
				}

				data = (msgdata_t *) msg;
				flash_writeData(data->offset, data->buff, data->size);
				proc_respond(flash_common.port, EOK, NULL, 0);
				break;

			case MSG_DEVCTL:
				if (size != sizeof(flashdevctl_t)) {
					proc_respond(flash_common.port, EINVAL, NULL, 0);
					break;
				}

				devctl = (flashdevctl_t *) msg;
				switch (devctl->type) {
					case FLASH_EVENT_GET:
						event_read(devctl, &event);
						proc_respond(flash_common.port, EOK, &event, sizeof(event));
						break;

					case FLASH_EVENT_SET:
						event_write(devctl);
						proc_respond(flash_common.port, EOK, NULL, 0);
						break;

					case FLASH_LOG_GET:
						log_read(&log);
						proc_respond(flash_common.port, EOK, &log, sizeof(log));
						break;

					case FLASH_LOG_SET:
						log_write(devctl);
						proc_respond(flash_common.port, EOK, NULL, 0);
						break;

					case FLASH_EEPROM_INFO_GET:
						info.addr = eeprom_freeAddr();
						info.size = eeprom_freeSize();
						proc_respond(flash_common.port, EOK, &info, sizeof(info));
						break;

					case FLASH_BANK_GET:
						bank = flash_activeBank();
						proc_respond(flash_common.port, EOK, &bank, sizeof(bank));
						break;

					case FLASH_BANK_BREAK:
						flash_bankBreak();
						proc_respond(flash_common.port, EOK, NULL, 0);
						break;

					case FLASH_ATOM_COPY:
						res = _flash_atomCopy(devctl->atomcpy.dest, devctl->atomcpy.src, devctl->atomcpy.len);
						proc_respond(flash_common.port, res, NULL, 0);
						break;
				}

				break;

			default:
				proc_respond(flash_common.port, EINVAL, NULL, 0);
				break;
		}
	}
}


static inline int flash_isValidAddress(u32 addr)
{
	if (program_isValidAddress(addr))
		return 1;

	if (eeprom_isValidAdress(addr))
		return 1;

	if (ob_isValidAdress(addr))
		return 1;

	return 0;
}


int flash_eventRead(int idx, flashevent_t *event)
{
	flashdevctl_t devctl;

	if (idx < 1 || idx > FLASH_EVENT_COUNT)
		return -EINVAL;

	devctl.type = FLASH_EVENT_GET;
	devctl.idx = idx - 1;

	return proc_send(flash_common.port, MSG_DEVCTL, &devctl, sizeof(devctl), MSG_NORMAL, event, sizeof(flashevent_t));
}


int flash_eventWrite(flashevent_t *event)
{
	flashdevctl_t devctl;
	devctl.type = FLASH_EVENT_SET;
	devctl.event = *event;

	return proc_send(flash_common.port, MSG_DEVCTL, &devctl, sizeof(devctl), MSG_NORMAL, NULL, 0);
}


int flash_logRead(flashlog_t *log)
{
	flashdevctl_t devctl;
	devctl.type = FLASH_LOG_GET;

	return proc_send(flash_common.port, MSG_DEVCTL, &devctl, sizeof(devctl), MSG_NORMAL, log, sizeof(flashlog_t));
}


int flash_logWrite(flashlog_t *log)
{
	flashdevctl_t devctl;
	devctl.type = FLASH_LOG_SET;
	devctl.log = *log;

	return proc_send(flash_common.port, MSG_DEVCTL, &devctl, sizeof(devctl), MSG_NORMAL, NULL, 0);
}


int flash_eepromInfo(unsigned int *addr, size_t *size)
{
	int res;
	flashdevctl_t devctl;
	flasheeprominfo_t info;

	devctl.type = FLASH_EEPROM_INFO_GET;

	res = proc_send(flash_common.port, MSG_DEVCTL, &devctl, sizeof(devctl), MSG_NORMAL, &info, sizeof(info));
	if (res < 0)
		return res;

	*addr = info.addr;
	*size = info.size;

	return EOK;
}


int flash_read(void *buff, size_t size, u32 addr)
{
	msgdata_t data;

	if (!flash_isValidAddress(addr))
		return -EINVAL;

	data.offset = addr;
	data.size = size;

	return proc_send(flash_common.port, MSG_READ, &data, sizeof(data), MSG_NORMAL, buff, size);
}


int flash_write(void *buff, size_t size, u32 addr)
{
	msgdata_t data;

	if (!flash_isValidAddress(addr))
		return -EINVAL;

	data.offset = addr;
	data.buff = buff;
	data.size = size;

	return proc_send(flash_common.port, MSG_WRITE, &data, sizeof(data), MSG_NORMAL, NULL, 0);
}


int flash_getActiveBank(int *bank)
{
	flashdevctl_t devctl;
	devctl.type = FLASH_BANK_GET;

	return proc_send(flash_common.port, MSG_DEVCTL, &devctl, sizeof(devctl), MSG_NORMAL, bank, sizeof(int));
}

int flash_breakActiveBank(void)
{
	flashdevctl_t devctl;
	devctl.type = FLASH_BANK_BREAK;

	return proc_send(flash_common.port, MSG_DEVCTL, &devctl, sizeof(devctl), MSG_NORMAL, NULL, 0);
}


int flash_atomCopy(u32 dest, u32 src, size_t len)
{
	flashdevctl_t devctl;

	devctl.type = FLASH_ATOM_COPY;
	devctl.atomcpy.dest = dest;
	devctl.atomcpy.src = src;
	devctl.atomcpy.len = len;

	return proc_send(flash_common.port, MSG_DEVCTL, &devctl, sizeof(devctl), MSG_NORMAL, NULL, 0);
}


void flash_init(void)
{
	flash_common.flash = (void *) 0x40023c00;
	flash_common.iwdg = (void *)0x40003000;

	proc_lockInit(&flash_common.lock);

	flash_clearFlags();

	proc_portCreate(&flash_common.port);
	proc_portRegister(flash_common.port, "/flashdrv");
	proc_threadCreate(NULL, flash_thread, NULL, 2, 1024, NULL, 0, NULL);
}


void event_read(flashdevctl_t *devctl, flashevent_t *event)
{
	int currIdx = log_common.area[EVENTS].currIdx;
	int diffIdx = (currIdx >= devctl->idx) ? (currIdx - devctl->idx) : (log_common.area[EVENTS].entryCount - (devctl->idx - currIdx));

	u32 addr = log_common.area[EVENTS].addr + diffIdx * log_common.area[EVENTS].entrySize;

	flash_readData(addr, (char *)event, log_common.area[EVENTS].entrySize);
}


void event_write(flashdevctl_t *devctl)
{
	u32 addr;

	log_common.area[EVENTS].currIdx += 1;
	log_common.area[EVENTS].currIdx %= log_common.area[EVENTS].entryCount;
	addr = log_common.area[EVENTS].addr + log_common.area[EVENTS].currIdx * log_common.area[EVENTS].entrySize;
	devctl->event.id = ++log_common.area[EVENTS].currId;

	flash_writeData(addr, (char *)(&devctl->event), log_common.area[EVENTS].entrySize);
}


void log_read(flashlog_t *log)
{
	int currIdx = log_common.area[LOGS].currIdx;
	u32 addr = log_common.area[LOGS].addr + currIdx * log_common.area[LOGS].entrySize;

	flash_readData(addr, (char *)log, log_common.area[LOGS].entrySize);
}


void log_write(flashdevctl_t *devctl)
{
	u32 addr;

	log_common.area[LOGS].currIdx += 1;
	log_common.area[LOGS].currIdx %= log_common.area[LOGS].entryCount;
	addr = log_common.area[LOGS].addr + log_common.area[LOGS].currIdx * log_common.area[LOGS].entrySize;
	devctl->event.id = ++log_common.area[LOGS].currId;

	flash_writeData(addr, (char *)(&devctl->event), log_common.area[LOGS].entrySize);
}


u32 eeprom_freeAddr(void)
{
	if (log_common.area[0].addr > log_common.area[1].addr)
		return log_common.area[0].addr + sizeof(u32) + log_common.area[0].entrySize * log_common.area[0].entryCount;
	else
		return log_common.area[1].addr + sizeof(u32) + log_common.area[1].entrySize * log_common.area[1].entryCount;
}


size_t eeprom_freeSize(void)
{
	size_t size = FLASH_EEPROM_SIZE;

	size -= 2 * sizeof(u32);
	size -= log_common.area[0].entryCount * log_common.area[0].entrySize;
	size -= log_common.area[1].entryCount * log_common.area[1].entrySize;

	return size;
}


void eeprom_init(void)
{
	u32 base;
	char buff[sizeof(u32)];
	char magic[] = { 0x12, 0x34, 0xab, 0xcd };
	int i, j;
	u64 id;

	/* Use eeprom from diffrent bank than flash we're executing from */
	if (!flash_activeBank())
		base = FLASH_EEPROM_2_ADDR;
	else
		base = FLASH_EEPROM_1_ADDR;

	log_common.area[EVENTS].addr = base;
	log_common.area[EVENTS].entrySize = sizeof(flashevent_t);
	log_common.area[EVENTS].entryCount = FLASH_EVENT_COUNT;
	log_common.area[LOGS].addr = base + sizeof(magic) + log_common.area[EVENTS].entrySize * log_common.area[EVENTS].entryCount;
	log_common.area[LOGS].entrySize = sizeof(flashlog_t);
	log_common.area[LOGS].entryCount = FLASH_LOG_COUNT;

	for (i = 0; i < 2; ++i) {
		/* Clear area if uninitialized. */
		flash_readData(log_common.area[i].addr + (log_common.area[i].entryCount * log_common.area[i].entrySize), buff, sizeof(buff));

		if (hal_memcmp(buff, magic, sizeof(magic)) != 0) {
			lib_printf("flash: Log area magic mismatch [%d], erasing...\n", i);

			for (j = 0; j < (log_common.area[i].entryCount * log_common.area[i].entrySize) + sizeof(magic); ++j)
				eeprom_eraseByte(log_common.area[i].addr + j);

			flash_writeData(log_common.area[i].addr + (log_common.area[i].entryCount * log_common.area[i].entrySize), magic, sizeof(magic));

			log_common.area[i].currId = 0;
			log_common.area[i].currIdx = 0;
			continue;
		}

		/* Find last log. */
		log_common.area[i].currId = 0;
		for (j = log_common.area[i].entryCount - 1; j >= 0; --j) {
			id = *((u64 *) (log_common.area[i].addr + (j * log_common.area[i].entrySize)));
			if (id >= log_common.area[i].currId) {
				log_common.area[i].currId = id;
				log_common.area[i].currIdx = j;
			}
		}
	}
}
