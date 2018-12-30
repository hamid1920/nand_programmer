/*  Copyright (C) 2017 Bogdan Bogush <bogdan.s.bogush@gmail.com>
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 */

#include "nand_programmer.h"
#include "nand_bad_block.h"
#include "fsmc_nand.h"
#include "chip_db.h"
#include "led.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#define NP_PACKET_BUF_SIZE 64
#define NP_MAX_PAGE_SIZE 0x0800
#define NP_WRITE_ACK_BYTES 1984
#define NP_NAND_TIMEOUT 0x1000000

#define NP_NAND_GOOD_BLOCK_MARK 0xFF

typedef enum
{
    NP_CMD_NAND_READ_ID = 0x00,
    NP_CMD_NAND_ERASE   = 0x01,
    NP_CMD_NAND_READ    = 0x02,
    NP_CMD_NAND_WRITE_S = 0x03,
    NP_CMD_NAND_WRITE_D = 0x04,
    NP_CMD_NAND_WRITE_E = 0x05,
    NP_CMD_NAND_SELECT  = 0x06,
    NP_CMD_NAND_READ_BB = 0x07,
    NP_CMD_NAND_LAST    = 0x08,
} np_cmd_code_t;

enum
{
    NP_ERR_INTERNAL       = -1,
    NP_ERR_ADDR_EXCEEDED  = -100,
    NP_ERR_ADDR_INVALID   = -101,
    NP_ERR_ADDR_NOT_ALIGN = -102,
    NP_ERR_NAND_WR        = -103,
    NP_ERR_NAND_RD        = -104,
    NP_ERR_NAND_ERASE     = -105,
    NP_ERR_CHIP_NOT_SEL   = -106,
    NP_ERR_CHIP_NOT_FOUND = -107,
    NP_ERR_CMD_DATA_SIZE  = -108,
    NP_ERR_CMD_INVALID    = -109,
    NP_ERR_BUF_OVERFLOW   = -110,
    NP_ERR_LEN_NOT_ALIGN  = -111,
    NP_ERR_LEN_EXCEEDED   = -112,
    NP_ERR_LEN_INVALID    = -113,
};

typedef struct __attribute__((__packed__))
{
    np_cmd_code_t code;
} np_cmd_t;

typedef struct __attribute__((__packed__))
{
    np_cmd_t cmd;
    uint32_t addr;
    uint32_t len;
} np_erase_cmd_t;

typedef struct __attribute__((__packed__))
{
    np_cmd_t cmd;
    uint32_t addr;
    uint32_t len;
} np_write_start_cmd_t;

typedef struct __attribute__((__packed__))
{
    np_cmd_t cmd;
    uint8_t len;
    uint8_t data[];
} np_write_data_cmd_t;

typedef struct __attribute__((__packed__))
{
    np_cmd_t cmd;
} np_write_end_cmd_t;

typedef struct __attribute__((__packed__))
{
    np_cmd_t cmd;
    uint32_t addr;
    uint32_t len;
} np_read_cmd_t;

typedef struct __attribute__((__packed__))
{
    np_cmd_t cmd;
    uint32_t chip_num;
} np_select_cmd_t;

enum
{
    NP_RESP_DATA   = 0x00,
    NP_RESP_STATUS = 0x01,
};

typedef struct __attribute__((__packed__))
{
    uint8_t code;
    uint8_t info;
    uint8_t data[];
} np_resp_t;

enum
{
    NP_STATUS_OK        = 0x00,
    NP_STATUS_ERROR     = 0x01,
    NP_STATUS_BAD_BLOCK = 0x02,
    NP_STATUS_WRITE_ACK = 0x03,
};

typedef struct __attribute__((__packed__))
{
    np_resp_t header;
    nand_id_t nand_id;
} np_resp_id_t;

typedef struct __attribute__((__packed__))
{
    np_resp_t header;
    uint32_t addr;
} np_resp_bad_block_t;

typedef struct __attribute__((__packed__))
{
    np_resp_t header;
    uint32_t bytes_ack;
} np_resp_write_ack_t;

typedef struct __attribute__((__packed__))
{
    np_resp_t header;
    uint8_t err_code;
} np_resp_err_t;

typedef struct
{
    uint32_t addr;
    int is_valid;
} np_prog_addr_t;

typedef struct
{
    uint8_t buf[NP_MAX_PAGE_SIZE];
    uint32_t page;
    uint32_t offset;
} np_page_t;

typedef struct
{
    uint8_t *rx_buf;
    uint32_t addr;
    uint32_t len;
    int addr_is_set;
    np_page_t page;
    uint32_t bytes_written;
    uint32_t bytes_ack;
    int nand_wr_in_progress;
    uint32_t nand_timeout;
    chip_info_t *chip_info;
} np_prog_t;

typedef struct
{
    int id;
    int (*exec)(np_prog_t *prog);
} np_cmd_handler_t;

static np_comm_cb_t *np_comm_cb;

uint8_t np_packet_send_buf[NP_PACKET_BUF_SIZE];

static int np_send_ok_status()
{
    np_resp_t status = { NP_RESP_STATUS, NP_STATUS_OK };
    size_t len = sizeof(status);

    if (np_comm_cb)
        np_comm_cb->send((uint8_t *)&status, len);

    return 0;
}

static int np_send_error(uint8_t err_code)
{
    np_resp_t status = { NP_RESP_STATUS, NP_STATUS_ERROR };
    np_resp_err_t err_status = { status, err_code };
    size_t len = sizeof(err_status);

    if (np_comm_cb)
        np_comm_cb->send((uint8_t *)&err_status, len);

    return 0;
}

static int np_send_bad_block_info(uint32_t addr)
{
    np_resp_t resp_header = { NP_RESP_STATUS, NP_STATUS_BAD_BLOCK };
    np_resp_bad_block_t bad_block = { resp_header, addr };

    if (np_comm_cb->send((uint8_t *)&bad_block, sizeof(bad_block)))
        return -1;

    return 0;
}

static int _np_cmd_nand_read_id(np_prog_t *prog)
{
    np_resp_id_t resp;
    size_t resp_len = sizeof(resp);

    DEBUG_PRINT("Read ID command\r\n");

    resp.header.code = NP_RESP_DATA;
    resp.header.info = resp_len - sizeof(resp.header);
    nand_read_id(&resp.nand_id);

    if (np_comm_cb)
        np_comm_cb->send((uint8_t *)&resp, resp_len);

    return 0;
}

static int np_cmd_nand_read_id(np_prog_t *prog)
{
    int ret;

    led_rd_set(true);
    ret = _np_cmd_nand_read_id(prog);
    led_rd_set(false);

    return ret;
}

static int np_nand_erase(np_prog_t *prog, uint32_t page)
{
    uint32_t status;
    uint32_t addr = page * prog->chip_info->page_size;
    
    DEBUG_PRINT("NAND erase at 0x%lx\r\n", addr);

    status = nand_erase_block(page);
    switch (status)
    {
    case NAND_READY:
        break;
    case NAND_ERROR:
        if (np_send_bad_block_info(addr))
            return -1;
        break;
    case NAND_TIMEOUT_ERROR:
        ERROR_PRINT("NAND erase timeout at 0x%lx\r\n", addr);
        break;
    default:
        ERROR_PRINT("Unknown NAND status\r\n");
        return -1;
    }

    return 0;
}

static int _np_cmd_nand_erase(np_prog_t *prog)
{
    int is_bad;
    uint32_t addr, page, pages_in_block, len;
    np_erase_cmd_t *erase_cmd = (np_erase_cmd_t *)prog->rx_buf;

    len = erase_cmd->len;
    addr = erase_cmd->addr;

    DEBUG_PRINT("Erase at 0x%lx %lx bytes command\r\n", addr, len);

    if (addr & (prog->chip_info->block_size - 1))
    {
        ERROR_PRINT("Address 0x%lx is not aligned to block size 0x%lx\r\n",
            addr, prog->chip_info->block_size);
        return NP_ERR_ADDR_NOT_ALIGN;
    }

    if (!len)
    {
        ERROR_PRINT("Length is 0\r\n");
        return NP_ERR_LEN_INVALID;
    }

    if (len & (prog->chip_info->block_size - 1))
    {
        ERROR_PRINT("Length 0x%lx is not aligned to block size 0x%lx\r\n",
            len, prog->chip_info->block_size);
        return NP_ERR_LEN_NOT_ALIGN;
    }

    if (addr + len > prog->chip_info->size)
    {
        ERROR_PRINT("Erase address exceded 0x%lx+0x%lx is more then chip size "
            "0x%lx\r\n", addr, len, prog->chip_info->size);
        return NP_ERR_ADDR_EXCEEDED;
    }

    page = addr / prog->chip_info->page_size;
    pages_in_block = prog->chip_info->block_size / prog->chip_info->page_size;

    while (len)
    {
        if (addr >= prog->chip_info->size)
        {
            ERROR_PRINT("Erase address 0x%lx is more then chip size 0x%lx\r\n",
                addr, prog->chip_info->size);
            return NP_ERR_ADDR_EXCEEDED;
        }

        if ((is_bad = nand_bad_block_table_lookup(addr)))
        {
            DEBUG_PRINT("Skipped bad block at 0x%lx\r\n", addr);
            if (np_send_bad_block_info(addr))
                return -1;
        }

        if (!is_bad && np_nand_erase(prog, page))
            return NP_ERR_NAND_ERASE;

        addr += prog->chip_info->block_size;
        page += pages_in_block;
        /* On partial erase do not count bad blocks */
        if (!is_bad || (is_bad && erase_cmd->len == prog->chip_info->size))
            len -= prog->chip_info->block_size;
    }

    return np_send_ok_status();
}

static int np_cmd_nand_erase(np_prog_t *prog)
{
    int ret;

    led_wr_set(true);
    ret = _np_cmd_nand_erase(prog);
    led_wr_set(false);

    return ret;
}

static int np_send_write_ack(uint32_t bytes_ack)
{
    np_resp_t resp_header = { NP_RESP_STATUS, NP_STATUS_WRITE_ACK };
    np_resp_write_ack_t write_ack = { resp_header, bytes_ack };

    if (np_comm_cb->send((uint8_t *)&write_ack, sizeof(write_ack)))
        return -1;

    return 0;
}

static int np_cmd_nand_write_start(np_prog_t *prog)
{
    uint32_t addr, len;

    np_write_start_cmd_t *write_start_cmd =
        (np_write_start_cmd_t *)prog->rx_buf;

    addr = write_start_cmd->addr;
    len = write_start_cmd->len;
    DEBUG_PRINT("Write at 0x%lx 0x%lx bytes command\r\n", addr, len);

    if (addr + len > prog->chip_info->size)
    {
        ERROR_PRINT("Write address 0x%lx+0x%lx is more then chip size "
            "0x%lx\r\n", addr, len, prog->chip_info->size);
        return NP_ERR_ADDR_EXCEEDED;
    }

    if (addr & (prog->chip_info->page_size - 1))
    {
        ERROR_PRINT("Address 0x%lx is not aligned to page size 0x%lx\r\n",
            addr, prog->chip_info->page_size);
        return NP_ERR_ADDR_NOT_ALIGN;
    }

    if (!len)
    {
        ERROR_PRINT("Length is 0\r\n");
        return NP_ERR_LEN_INVALID;
    }

    if (len & (prog->chip_info->page_size - 1))
    {
        ERROR_PRINT("Length 0x%lx is not aligned to page size 0x%lx\r\n",
            len, prog->chip_info->page_size);
        return NP_ERR_ADDR_NOT_ALIGN;
    }

    prog->addr = addr;
    prog->len = len;
    prog->addr_is_set = 1;

    prog->page.page = addr / prog->chip_info->page_size;
    prog->page.offset = 0;

    prog->bytes_written = 0;
    prog->bytes_ack = 0;

    return np_send_ok_status();
}

static int np_nand_handle_status(np_prog_t *prog)
{
    switch (nand_read_status())
    {
    case NAND_ERROR:
        if (np_send_bad_block_info(prog->addr))
            return -1;
    case NAND_READY:
        prog->nand_wr_in_progress = 0;
        prog->nand_timeout = 0;
        break;
    case NAND_BUSY:
        if (++prog->nand_timeout == NP_NAND_TIMEOUT)
        {
            ERROR_PRINT("NAND write timeout at 0x%lx\r\n", prog->addr);
            prog->nand_wr_in_progress = 0;
            prog->nand_timeout = 0;
            return -1;
        }
        break;
    default:
        ERROR_PRINT("Unknown NAND status\r\n");
        prog->nand_wr_in_progress = 0;
        prog->nand_timeout = 0;
        return -1;
    }

    return 0;
}

static int np_nand_write(np_prog_t *prog, chip_info_t *chip_info)
{   
    if (prog->nand_wr_in_progress)
    {
        DEBUG_PRINT("Wait for previous NAND write\r\n");
        do
        {
            if (np_nand_handle_status(prog))
                return -1;
        }
        while (prog->nand_wr_in_progress);
    }

    DEBUG_PRINT("NAND write at 0x%lx %lu bytes\r\n", prog->addr,
        chip_info->page_size);

    nand_write_page_async(prog->page.buf, prog->page.page,
        chip_info->page_size);

    prog->nand_wr_in_progress = 1;

    return 0;
}

static int np_cmd_nand_write_data(np_prog_t *prog)
{
    uint32_t write_len, bytes_left, len;
    np_write_data_cmd_t *write_data_cmd = (np_write_data_cmd_t *)prog->rx_buf;

    len = write_data_cmd->len;
    if (len + sizeof(np_write_data_cmd_t) > NP_PACKET_BUF_SIZE)
    {
        ERROR_PRINT("Data size is wrong 0x%lx\r\n", len);
        return NP_ERR_CMD_DATA_SIZE;
    }

    if (!prog->addr_is_set)
    {
        ERROR_PRINT("Write address is not set\r\n");
        return NP_ERR_ADDR_INVALID;
    }

    if (prog->addr >= prog->chip_info->size)
    {
        ERROR_PRINT("Write address 0x%lx is more then chip size 0x%lx\r\n",
            prog->addr, prog->chip_info->size);
        return NP_ERR_ADDR_EXCEEDED;
    }

    if (prog->page.offset + len > prog->chip_info->page_size)
        write_len = prog->chip_info->page_size - prog->page.offset;
    else
        write_len = len;

    memcpy(prog->page.buf + prog->page.offset, write_data_cmd->data, write_len);
    prog->page.offset += write_len;

    if (prog->page.offset == prog->chip_info->page_size)
    {
        if (np_nand_write(prog, prog->chip_info))
            return NP_ERR_NAND_WR;

        prog->addr += prog->chip_info->page_size;
        prog->page.page++;
        prog->page.offset = 0;
    }

    bytes_left = len - write_len;
    if (bytes_left)
    {
        memcpy(prog->page.buf, write_data_cmd->data + write_len, bytes_left);
        prog->page.offset += bytes_left;
    }

    prog->bytes_written += len;
    if (prog->bytes_written - prog->bytes_ack >= prog->chip_info->page_size ||
        prog->bytes_written == prog->len)
    {
        if (np_send_write_ack(prog->bytes_written))
            return -1;
        prog->bytes_ack = prog->bytes_written;
    }

    if (prog->bytes_written > prog->len)
    {
        ERROR_PRINT("Actual write data length 0x%lx is more then 0x%lx\r\n",
            prog->bytes_written, prog->len);
        return NP_ERR_LEN_EXCEEDED;
    }

    return 0;
}

static int np_cmd_nand_write_end(np_prog_t *prog)
{
    prog->addr_is_set = 0;

    if (prog->page.offset)
    {
        ERROR_PRINT("Data of 0x%lx length was not written\r\n",
            prog->page.offset);
        return NP_ERR_NAND_WR;
    }

    return np_send_ok_status();
}

static int np_cmd_nand_write(np_prog_t *prog)
{
    np_cmd_t *cmd = (np_cmd_t *)prog->rx_buf;
    int ret = 0;

    switch (cmd->code)
    {
    case NP_CMD_NAND_WRITE_S:
        led_wr_set(true);
        ret = np_cmd_nand_write_start(prog);
        break;
    case NP_CMD_NAND_WRITE_D:
        ret = np_cmd_nand_write_data(prog);
        break;
    case NP_CMD_NAND_WRITE_E:
        ret = np_cmd_nand_write_end(prog);
        led_wr_set(false);
        break;
    default:
        break;
    }

    return ret;
}

static int np_nand_read(uint32_t addr, np_page_t *page,
    chip_info_t *chip_info)
{
    uint32_t status;

    DEBUG_PRINT("NAND read at 0x%lx\r\n", addr);    
    
    status = nand_read_page(page->buf, page->page, chip_info->page_size);
    switch (status)
    {
    case NAND_READY:
        break;
    case NAND_ERROR:
        if (np_send_bad_block_info(addr))
            return -1;
        break;
    case NAND_TIMEOUT_ERROR:
        ERROR_PRINT("NAND write timeout at 0x%lx\r\n", addr);
        break;
    default:
        ERROR_PRINT("Unknown NAND status\r\n");
        return -1;
    }

    return 0;
}

static int _np_cmd_nand_read(np_prog_t *prog)
{
    uint32_t addr, len, write_len;
    static np_page_t page;
    uint32_t resp_header_size = offsetof(np_resp_t, data);
    uint32_t tx_data_len = sizeof(np_packet_send_buf) - resp_header_size;
    np_read_cmd_t *read_cmd = (np_read_cmd_t *)prog->rx_buf;
    np_resp_t *resp = (np_resp_t *)np_packet_send_buf;

    addr = read_cmd->addr;
    len = read_cmd->len;    
    DEBUG_PRINT("Read at 0x%lx 0x%lx bytes command\r\n", addr, len);

    if (addr + len > prog->chip_info->size)
    {
        ERROR_PRINT("Read address 0x%lx+0x%lx is more then chip size 0x%lx\r\n",
            addr, len, prog->chip_info->size);
        return NP_ERR_ADDR_EXCEEDED;
    }

    if (addr & (prog->chip_info->page_size - 1))
    {
        ERROR_PRINT("Read address 0x%lx is not aligned to page size 0x%lx\r\n",
            addr, prog->chip_info->page_size);
        return NP_ERR_ADDR_NOT_ALIGN;
    }

    if (!len)
    {
        ERROR_PRINT("Length is 0\r\n");
        return NP_ERR_LEN_INVALID;
    }

    if (len & (prog->chip_info->page_size - 1))
    {
        ERROR_PRINT("Read length 0x%lx is not aligned to page size 0x%lx\r\n",
            len, prog->chip_info->page_size);
        return NP_ERR_LEN_NOT_ALIGN;
    }

    page.page = addr / prog->chip_info->page_size;
    page.offset = 0;

    resp->code = NP_RESP_DATA;

    while (len)
    {
        if (np_nand_read(addr, &page, prog->chip_info))
            return NP_ERR_NAND_RD;

        while (page.offset < prog->chip_info->page_size && len)
        {
            if (prog->chip_info->page_size - page.offset >= tx_data_len)
                write_len = tx_data_len;
            else
                write_len = prog->chip_info->page_size - page.offset;

            if (write_len > len)
                write_len = len;
 
            memcpy(resp->data, page.buf + page.offset, write_len);

            while (!np_comm_cb->send_ready());

            resp->info = write_len;
            if (np_comm_cb->send(np_packet_send_buf,
                resp_header_size + write_len))
            {
                return -1;
            }

            page.offset += write_len;
            len -= write_len;
        }

        if (len)
        {
            addr += prog->chip_info->page_size;
            if (addr >= prog->chip_info->size)
            {
                ERROR_PRINT("Read address 0x%lx is more then chip size 0x%lx",
                    addr, prog->chip_info->page_size);
                return NP_ERR_ADDR_EXCEEDED;
            }
            page.page++;
            page.offset = 0;
        }
    }

    return 0;
}

static int np_cmd_nand_read(np_prog_t *prog)
{
    int ret;

    led_rd_set(true);
    ret = _np_cmd_nand_read(prog);
    led_rd_set(false);

    return ret;
}

static int np_cmd_nand_select(np_prog_t *prog)
{
    np_select_cmd_t *select_cmd = (np_select_cmd_t *)prog->rx_buf;

    DEBUG_PRINT("Chip select ID %lu command\r\n", select_cmd->chip_num);

    if (!chip_select(select_cmd->chip_num))
    {
        nand_init();
        nand_bad_block_table_init();
        prog->chip_info = chip_info_selected_get();
    }
    else
    {
        prog->chip_info = NULL;

        ERROR_PRINT("Chip ID %lu not found\r\n", select_cmd->chip_num);
        return NP_ERR_CHIP_NOT_FOUND;
    }

    return np_send_ok_status();
}

static int np_read_bad_block_info_from_page(np_prog_t *prog, uint32_t block,
    uint32_t page, chip_info_t *chip_info, bool *is_bad)
{
    uint8_t bad_block_data;
    uint32_t status, addr = block * chip_info->block_size;

    status = nand_read_data(&bad_block_data, page, chip_info->page_size,
        sizeof(bad_block_data));
    switch (status)
    {
    case NAND_READY:
        break;
    case NAND_ERROR:
        ERROR_PRINT("NAND read bad block info error at 0x%lx\r\n", addr);
        return NP_ERR_NAND_RD;
    case NAND_TIMEOUT_ERROR:
        ERROR_PRINT("NAND read timeout at 0x%lx\r\n", addr);
        return NP_ERR_NAND_RD;
    default:
        ERROR_PRINT("Unknown NAND status\r\n");
        return NP_ERR_NAND_RD;
    }

    if (bad_block_data != NP_NAND_GOOD_BLOCK_MARK)
    {
        *is_bad = true;
        if (np_send_bad_block_info(addr))
            return -1;
        if (nand_bad_block_table_add(addr))
            return -1;
    }
    else
        *is_bad = false;

    return 0;
}

static int _np_cmd_read_bad_blocks(np_prog_t *prog)
{
    bool is_bad;
    uint32_t block, block_num, page_num, page;

    block_num = prog->chip_info->size / prog->chip_info->block_size;
    page_num = prog->chip_info->block_size / prog->chip_info->page_size;

    /* Bad block - not 0xFF value in the first or second page in the block at
     * zero offset in the page spare area
     */
    for (block = 0; block < block_num; block++)
    {
        page = block * page_num;
        if (np_read_bad_block_info_from_page(prog, block, page, prog->chip_info,
            &is_bad))
        {
            return -1;
        }

        if (!is_bad && np_read_bad_block_info_from_page(prog, block, page + 1,
            prog->chip_info, &is_bad))
        {
            return -1;
        }
    }

    return np_send_ok_status();
}

int np_cmd_read_bad_blocks(np_prog_t *prog)
{
    int ret;

    led_rd_set(true);    
    ret = _np_cmd_read_bad_blocks(prog);
    led_rd_set(false);

    return ret;
}

static np_cmd_handler_t cmd_handler[] =
{
    { NP_CMD_NAND_READ_ID, np_cmd_nand_read_id },
    { NP_CMD_NAND_ERASE, np_cmd_nand_erase },
    { NP_CMD_NAND_READ, np_cmd_nand_read },
    { NP_CMD_NAND_WRITE_S, np_cmd_nand_write },
    { NP_CMD_NAND_WRITE_D, np_cmd_nand_write },
    { NP_CMD_NAND_WRITE_E, np_cmd_nand_write },
    { NP_CMD_NAND_SELECT, np_cmd_nand_select },
    { NP_CMD_NAND_READ_BB, np_cmd_read_bad_blocks },
};

static bool np_cmd_is_valid(np_cmd_code_t code)
{
    return code >= 0 && code < NP_CMD_NAND_LAST;
}

static int np_cmd_handler(np_prog_t *prog)
{
    np_cmd_t *cmd = (np_cmd_t *)prog->rx_buf;

    if (!prog->chip_info && cmd->code != NP_CMD_NAND_SELECT)
    {
        ERROR_PRINT("Chip is not selected\r\n");
        return NP_ERR_CHIP_NOT_SEL;
    }

    if (!np_cmd_is_valid(cmd->code))
    {
        ERROR_PRINT("Invalid cmd code %d\r\n", cmd->code);
        return NP_ERR_CMD_INVALID;
    }

    return cmd_handler[cmd->code].exec(prog);
}

static void np_packet_handler(np_prog_t *prog)
{
    int ret;

    do
    {
        np_comm_cb->peek(&prog->rx_buf);

        if (!prog->rx_buf)
            break;

        ret = np_cmd_handler(prog);

        np_comm_cb->consume();

        if (ret < 0)
            np_send_error(-ret);
    }
    while (1);
}

static void np_nand_handler(np_prog_t *prog)
{
    if (prog->nand_wr_in_progress)
    {
        if (np_nand_handle_status(prog))
            np_send_error(NP_ERR_NAND_WR);
    }
}

void np_handler()
{
    static np_prog_t prog;

    np_packet_handler(&prog);
    np_nand_handler(&prog);
}

int np_comm_register(np_comm_cb_t *cb)
{
    np_comm_cb = cb;

    return 0;
}

void np_comm_unregister(np_comm_cb_t *cb)
{
    if (np_comm_cb == cb)
        np_comm_cb = NULL;
}
