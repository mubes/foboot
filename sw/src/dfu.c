/*
 * Fadecandy DFU Bootloader
 * 
 * Copyright (c) 2013 Micah Elizabeth Scott
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdbool.h>
#include <string.h>

#include <toboot-api.h>
#include <toboot-internal.h>
#include <dfu.h>
#include <rgb.h>

#define ERASE_SIZE 65536 // Erase block size (in bytes)
#define WRITE_SIZE 256 // Number of bytes we can write

#include <spi.h>
extern struct ff_spi *spi; // Defined in main.c

// Internal flash-programming state machine
static unsigned fl_current_addr = 0;
static enum {
    flsIDLE = 0,
    flsERASING,
    flsPROGRAMMING
} fl_state;

static dfu_state_t dfu_state = dfuIDLE;
static dfu_status_t dfu_status = OK;
static unsigned dfu_poll_timeout_ms = 1;

static uint32_t dfu_buffer[DFU_TRANSFER_SIZE/4];
static uint32_t dfu_buffer_offset;
static uint32_t dfu_bytes_remaining;

// Memory offset we're uploading to.
static uint32_t dfu_target_address;

static void set_state(dfu_state_t new_state, dfu_status_t new_status) {
    if (new_state == dfuIDLE)
        rgb_mode_idle();
    else if (new_status != OK)
        rgb_mode_error();
    else if (new_state == dfuMANIFEST_WAIT_RESET)
        rgb_mode_done();
    else
        rgb_mode_writing();
    dfu_state = new_state;
    dfu_status = new_status;
}

static bool ftfl_busy()
{
    // Is the flash memory controller busy?
    return spiIsBusy(spi);
}

static void ftfl_busy_wait()
{
    // Wait for the flash memory controller to finish any pending operation.
    while (ftfl_busy())
        ;//watchdog_refresh();
}

static void ftfl_begin_erase_sector(uint32_t address)
{
    ftfl_busy_wait();
    // Only erase if it's on the page boundry.
    if ((address & ~(ERASE_SIZE - 1) ) == address)
        spiBeginErase64(spi, address);
    fl_state = flsERASING;
}

static void ftfl_write_more_bytes(void)
{
    uint32_t bytes_to_write = WRITE_SIZE;
    if (dfu_bytes_remaining < bytes_to_write)
        bytes_to_write = dfu_bytes_remaining;
    ftfl_busy_wait();
    spiBeginWrite(spi, dfu_target_address, &dfu_buffer[dfu_buffer_offset], bytes_to_write);

    dfu_bytes_remaining -= bytes_to_write;
    dfu_target_address += bytes_to_write;
    dfu_buffer_offset += bytes_to_write;
}

static void ftfl_begin_program_section(uint32_t address)
{
    // Write the buffer word to the currently selected address.
    // Note that after this is done, the address is incremented by 4.
    dfu_buffer_offset = 0;
    dfu_target_address = address;
    ftfl_write_more_bytes();
}

static uint32_t address_for_block(unsigned blockNum)
{
    static const uint32_t starting_offset = 262144;
    return starting_offset + (blockNum * WRITE_SIZE);
}

void dfu_init(void)
{
    return;
}

uint8_t dfu_getstate(void)
{
    return dfu_state;
}

unsigned last_blockNum;
unsigned last_blockLength;
unsigned last_packetOffset;
unsigned last_packetLength;
bool dfu_download(unsigned blockNum, unsigned blockLength,
                  unsigned packetOffset, unsigned packetLength, const uint8_t *data)
{
    // uint32_t i;
    last_packetLength = packetLength;
    last_packetOffset = packetOffset;
    last_blockLength = blockLength;
    last_blockNum = blockNum;

    if (packetOffset + packetLength > DFU_TRANSFER_SIZE ||
        packetOffset + packetLength > blockLength) {

        // Overflow!
        set_state(dfuERROR, errADDRESS);
        return false;
    }

    // Store more data...
    memcpy(((uint8_t *)dfu_buffer) + packetOffset, data, packetLength);

    if (packetOffset + packetLength != blockLength) {
        // Still waiting for more data.
        return true;
    }

    if (dfu_state != dfuIDLE && dfu_state != dfuDNLOAD_IDLE) {
        // Wrong state! Oops.
        set_state(dfuERROR, errSTALLEDPKT);
        return false;
    }

    if (ftfl_busy() || (fl_state != flsIDLE)) {
        // Flash controller shouldn't be busy now!
        set_state(dfuERROR, errWRITE);
        return false;
    }

    if (!blockLength) {
        // End of download
        set_state(dfuMANIFEST_SYNC, OK);
        return true;
    }

    // Start programming a block by erasing the corresponding flash sector
    fl_state = flsERASING;
    fl_current_addr = address_for_block(blockNum);
    dfu_bytes_remaining = blockLength;

#if 0
    // If it's the first block, figure out what we need to do in terms of erasing
    // data and programming the new file.
    if (blockNum == 0) {
        const struct toboot_configuration *old_config = tb_get_config();

        // Don't allow overwriting Toboot itself.
        if (fl_current_addr < tb_first_free_address()) {
            set_state(dfuERROR, errADDRESS);
            return false;
        }

        // Calculate generation number and hash
        if (tb_state.version == 2) {
            struct toboot_configuration *new_config = (struct toboot_configuration *)&dfu_buffer[0x94 / 4];

            // Update generation number
            new_config->reserved_gen = old_config->reserved_gen + 1;

            // Ensure we know this header is not fake
            new_config->config &= ~TOBOOT_CONFIG_FAKE;

            // Generate a valid signature
            tb_sign_config(new_config);
        }

        // If the old configuration requires that certain blocks be erased, do that.
        tb_state.clear_hi = old_config->erase_mask_hi;
        tb_state.clear_lo = old_config->erase_mask_lo;
        tb_state.clear_current = 0;

        // Ensure we don't erase Foboot itself
        for (i = 0; i < tb_first_free_sector(); i++) {
            if (i < 32)
                tb_state.clear_lo &= ~(1 << i);
            else
                tb_state.clear_hi &= ~(1 << i);
        }

        // If the newly-loaded program does not conform to Foboot V2.0, then look
        // for any existing programs on the flash and delete those sectors.
        // Because of boot priority, we need to ensure that no V2.0 applications
        // exist on flash.
        if (tb_state.version < 2) {
            for (i = tb_first_free_sector(); i < 64; i++) {
                if (tb_valid_signature_at_page(i) < 0)
                    continue;
                if (i < 32)
                    tb_state.clear_lo |= (1 << i);
                else
                    tb_state.clear_hi |= (1 << (i - 32));
            }
        }

        // If we still have sectors to clear, do that.  Otherwise,
        // go straight into loading the program.
        if (tb_state.clear_lo || tb_state.clear_hi) {
            tb_state.state = tbsCLEARING;
            tb_state.next_addr = fl_current_addr;
            pre_clear_next_block();
        }
        else {
            tb_state.state = tbsLOADING;
            ftfl_begin_erase_sector(fl_current_addr);
        }
    }
    else
#endif
    ftfl_begin_erase_sector(fl_current_addr);

    set_state(dfuDNLOAD_SYNC, OK);
    return true;
}

static void fl_state_poll(void)
{
    // Try to advance the state of our own flash programming state machine.

    if (spiIsBusy(spi))
        return;

    switch (fl_state) {

    case flsIDLE:
        break;

    case flsERASING:
        // // If we're still pre-clearing, continue with that.
        // if (tb_state.state == tbsCLEARING) {
        //     pre_clear_next_block();
        // }
        // // Done! Move on to programming the sector.
        // else {
        fl_state = flsPROGRAMMING;
        ftfl_begin_program_section(fl_current_addr);
        // }
        break;

    case flsPROGRAMMING:
        // Program more blocks, if applicable
        if (dfu_bytes_remaining)
            ftfl_write_more_bytes();
        else
            fl_state = flsIDLE;
        break;
    }
}

void dfu_poll(void)
{
    if ((dfu_state == dfuDNLOAD_SYNC) || (dfu_state == dfuDNBUSY))
        fl_state_poll();
}

bool dfu_getstatus(uint8_t status[8])
{
    switch (dfu_state) {

        case dfuDNLOAD_SYNC:
        case dfuDNBUSY:
            // Programming operation in progress. Advance our private flash state machine.
            // fl_state_poll();

            if (dfu_state == dfuERROR) {
                // An error occurred inside fl_state_poll();
            } else if (fl_state == flsIDLE) {
                set_state(dfuDNLOAD_IDLE, OK);
            } else {
                set_state(dfuDNBUSY, OK);
            }
            break;

        case dfuMANIFEST_SYNC:
            // Ready to reboot. The main thread will take care of this. Also let the DFU tool
            // know to leave us alone until this happens.
            set_state(dfuMANIFEST, OK);
            dfu_poll_timeout_ms = 10;
            break;

        case dfuMANIFEST:
            // Perform the reboot
            set_state(dfuMANIFEST_WAIT_RESET, OK);
            dfu_poll_timeout_ms = 1000;
            break;

        default:
            break;
    }

    status[0] = dfu_status;
    status[1] = dfu_poll_timeout_ms;
    status[2] = dfu_poll_timeout_ms >> 8;
    status[3] = dfu_poll_timeout_ms >> 16;
    status[4] = dfu_state;
    status[5] = 0;  // iString

    return true;
}

bool dfu_clrstatus(void)
{
    switch (dfu_state) {

    case dfuERROR:
        // Clear an error
        set_state(dfuIDLE, OK);
        return true;

    default:
        // Unexpected request
        set_state(dfuERROR, errSTALLEDPKT);
        return false;
    }
}

bool dfu_abort(void)
{
    set_state(dfuIDLE, OK);
    return true;
}