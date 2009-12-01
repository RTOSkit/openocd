/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2007,2008 Øyvind Harboe                                 *
 *   oyvind.harboe@zylin.com                                               *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk                                                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "flash.h"
#include "common.h"
#include "image.h"
#include "time_support.h"

static int flash_write_unlock(struct target *target, struct image *image, uint32_t *written, int erase, bool unlock);

/* flash drivers
 */
extern struct flash_driver lpc2000_flash;
extern struct flash_driver lpc288x_flash;
extern struct flash_driver lpc2900_flash;
extern struct flash_driver cfi_flash;
extern struct flash_driver at91sam3_flash;
extern struct flash_driver at91sam7_flash;
extern struct flash_driver str7x_flash;
extern struct flash_driver str9x_flash;
extern struct flash_driver aduc702x_flash;
extern struct flash_driver stellaris_flash;
extern struct flash_driver str9xpec_flash;
extern struct flash_driver stm32x_flash;
extern struct flash_driver tms470_flash;
extern struct flash_driver ecosflash_flash;
extern struct flash_driver ocl_flash;
extern struct flash_driver pic32mx_flash;
extern struct flash_driver avr_flash;
extern struct flash_driver faux_flash;

struct flash_driver *flash_drivers[] = {
	&lpc2000_flash,
	&lpc288x_flash,
	&lpc2900_flash,
	&cfi_flash,
	&at91sam7_flash,
	&at91sam3_flash,
	&str7x_flash,
	&str9x_flash,
	&aduc702x_flash,
	&stellaris_flash,
	&str9xpec_flash,
	&stm32x_flash,
	&tms470_flash,
	&ecosflash_flash,
	&ocl_flash,
	&pic32mx_flash,
	&avr_flash,
	&faux_flash,
	NULL,
};

struct flash_bank *flash_banks;

/* wafer thin wrapper for invoking the flash driver */
static int flash_driver_write(struct flash_bank *bank, uint8_t *buffer, uint32_t offset, uint32_t count)
{
	int retval;

	retval = bank->driver->write(bank, buffer, offset, count);
	if (retval != ERROR_OK)
	{
		LOG_ERROR("error writing to flash at address 0x%08" PRIx32 " at offset 0x%8.8" PRIx32 " (%d)",
			  bank->base, offset, retval);
	}

	return retval;
}

static int flash_driver_erase(struct flash_bank *bank, int first, int last)
{
	int retval;

	retval = bank->driver->erase(bank, first, last);
	if (retval != ERROR_OK)
	{
		LOG_ERROR("failed erasing sectors %d to %d (%d)", first, last, retval);
	}

	return retval;
}

int flash_driver_protect(struct flash_bank *bank, int set, int first, int last)
{
	int retval;

	retval = bank->driver->protect(bank, set, first, last);
	if (retval != ERROR_OK)
	{
		LOG_ERROR("failed setting protection for areas %d to %d (%d)", first, last, retval);
	}

	return retval;
}

static int jim_flash_banks(Jim_Interp *interp, int argc, Jim_Obj *const *argv)
{
	struct flash_bank *p;

	if (argc != 1) {
		Jim_WrongNumArgs(interp, 1, argv, "no arguments to flash_banks command");
		return JIM_ERR;
	}

	Jim_Obj *list = Jim_NewListObj(interp, NULL, 0);
	for (p = flash_banks; p; p = p->next)
	{
		Jim_Obj *elem = Jim_NewListObj(interp, NULL, 0);

		Jim_ListAppendElement(interp, elem, Jim_NewStringObj(interp, "name", -1));
		Jim_ListAppendElement(interp, elem, Jim_NewStringObj(interp, p->driver->name, -1));
		Jim_ListAppendElement(interp, elem, Jim_NewStringObj(interp, "base", -1));
		Jim_ListAppendElement(interp, elem, Jim_NewIntObj(interp, p->base));
		Jim_ListAppendElement(interp, elem, Jim_NewStringObj(interp, "size", -1));
		Jim_ListAppendElement(interp, elem, Jim_NewIntObj(interp, p->size));
		Jim_ListAppendElement(interp, elem, Jim_NewStringObj(interp, "bus_width", -1));
		Jim_ListAppendElement(interp, elem, Jim_NewIntObj(interp, p->bus_width));
		Jim_ListAppendElement(interp, elem, Jim_NewStringObj(interp, "chip_width", -1));
		Jim_ListAppendElement(interp, elem, Jim_NewIntObj(interp, p->chip_width));

		Jim_ListAppendElement(interp, list, elem);
	}

	Jim_SetResult(interp, list);

	return JIM_OK;
}

struct flash_bank *get_flash_bank_by_num_noprobe(int num)
{
	struct flash_bank *p;
	int i = 0;

	for (p = flash_banks; p; p = p->next)
	{
		if (i++ == num)
		{
			return p;
		}
	}
	LOG_ERROR("flash bank %d does not exist", num);
	return NULL;
}

int flash_get_bank_count(void)
{
	struct flash_bank *p;
	int i = 0;
	for (p = flash_banks; p; p = p->next)
	{
		i++;
	}
	return i;
}

struct flash_bank *get_flash_bank_by_name(const char *name)
{
	unsigned requested = get_flash_name_index(name);
	unsigned found = 0;

	struct flash_bank *bank;
	for (bank = flash_banks; NULL != bank; bank = bank->next)
	{
		if (strcmp(bank->name, name) == 0)
			return bank;
		if (!flash_driver_name_matches(bank->driver->name, name))
			continue;
		if (++found < requested)
			continue;
		return bank;
	}
	return NULL;
}

struct flash_bank *get_flash_bank_by_num(int num)
{
	struct flash_bank *p = get_flash_bank_by_num_noprobe(num);
	int retval;

	if (p == NULL)
		return NULL;

	retval = p->driver->auto_probe(p);

	if (retval != ERROR_OK)
	{
		LOG_ERROR("auto_probe failed %d\n", retval);
		return NULL;
	}
	return p;
}

COMMAND_HELPER(flash_command_get_bank, unsigned name_index,
		struct flash_bank **bank)
{
	const char *name = CMD_ARGV[name_index];
	*bank = get_flash_bank_by_name(name);
	if (*bank)
		return ERROR_OK;

	unsigned bank_num;
	COMMAND_PARSE_NUMBER(uint, name, bank_num);

	*bank = get_flash_bank_by_num(bank_num);
	if (!*bank)
	{
		command_print(CMD_CTX, "flash bank '%s' not found", name);
		return ERROR_INVALID_ARGUMENTS;
	}
	return ERROR_OK;
}


COMMAND_HANDLER(handle_flash_bank_command)
{
	if (CMD_ARGC < 7)
	{
		LOG_ERROR("usage: flash bank <name> <driver> "
				"<base> <size> <chip_width> <bus_width>");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}
	// save bank name and advance arguments for compatibility
	const char *bank_name = *CMD_ARGV++;
	CMD_ARGC--;

	struct target *target;
	if ((target = get_target(CMD_ARGV[5])) == NULL)
	{
		LOG_ERROR("target '%s' not defined", CMD_ARGV[5]);
		return ERROR_FAIL;
	}

	const char *driver_name = CMD_ARGV[0];
	for (unsigned i = 0; flash_drivers[i]; i++)
	{
		if (strcmp(driver_name, flash_drivers[i]->name) != 0)
			continue;

		/* register flash specific commands */
		if (NULL != flash_drivers[i]->commands)
		{
			int retval = register_commands(CMD_CTX, NULL,
					flash_drivers[i]->commands);
			if (ERROR_OK != retval)
			{
				LOG_ERROR("couldn't register '%s' commands",
						driver_name);
				return ERROR_FAIL;
			}
		}

		struct flash_bank *p, *c;
		c = malloc(sizeof(struct flash_bank));
		c->name = strdup(bank_name);
		c->target = target;
		c->driver = flash_drivers[i];
		c->driver_priv = NULL;
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], c->base);
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[2], c->size);
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[3], c->chip_width);
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[4], c->bus_width);
		c->num_sectors = 0;
		c->sectors = NULL;
		c->next = NULL;

		int retval;
		retval = CALL_COMMAND_HANDLER(flash_drivers[i]->flash_bank_command, c);
		if (ERROR_OK != retval)
		{
			LOG_ERROR("'%s' driver rejected flash bank at 0x%8.8" PRIx32,
					driver_name, c->base);
			free(c);
			return retval;
		}

		/* put flash bank in linked list */
		if (flash_banks)
		{
			int	bank_num = 0;
			/* find last flash bank */
			for (p = flash_banks; p && p->next; p = p->next) bank_num++;
			if (p)
				p->next = c;
			c->bank_number = bank_num + 1;
		}
		else
		{
			flash_banks = c;
			c->bank_number = 0;
		}

		return ERROR_OK;
	}

	/* no matching flash driver found */
	LOG_ERROR("flash driver '%s' not found", driver_name);
	return ERROR_FAIL;
}

COMMAND_HANDLER(handle_flash_info_command)
{
	struct flash_bank *p;
	uint32_t i = 0;
	int j = 0;
	int retval;

	if (CMD_ARGC != 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	unsigned bank_nr;
	COMMAND_PARSE_NUMBER(uint, CMD_ARGV[0], bank_nr);

	for (p = flash_banks; p; p = p->next, i++)
	{
		if (i != bank_nr)
			continue;

		char buf[1024];

		/* attempt auto probe */
		if ((retval = p->driver->auto_probe(p)) != ERROR_OK)
			return retval;

		command_print(CMD_CTX,
			      "#%" PRIi32 " : %s at 0x%8.8" PRIx32 ", size 0x%8.8" PRIx32 ", buswidth %i, chipwidth %i",
			      i,
			      p->driver->name,
			      p->base,
			      p->size,
			      p->bus_width,
			      p->chip_width);
		for (j = 0; j < p->num_sectors; j++)
		{
			char *protect_state;

			if (p->sectors[j].is_protected == 0)
				protect_state = "not protected";
			else if (p->sectors[j].is_protected == 1)
				protect_state = "protected";
			else
				protect_state = "protection state unknown";

			command_print(CMD_CTX,
				      "\t#%3i: 0x%8.8" PRIx32 " (0x%" PRIx32 " %" PRIi32 "kB) %s",
				      j,
				      p->sectors[j].offset,
				      p->sectors[j].size,
				      p->sectors[j].size >> 10,
				      protect_state);
		}

		*buf = '\0'; /* initialize buffer, otherwise it migh contain garbage if driver function fails */
		retval = p->driver->info(p, buf, sizeof(buf));
		command_print(CMD_CTX, "%s", buf);
		if (retval != ERROR_OK)
			LOG_ERROR("error retrieving flash info (%d)", retval);
	}

	return ERROR_OK;
}

COMMAND_HANDLER(handle_flash_probe_command)
{
	int retval;

	if (CMD_ARGC != 1)
	{
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	unsigned bank_nr;
	COMMAND_PARSE_NUMBER(uint, CMD_ARGV[0], bank_nr);
	struct flash_bank *p = get_flash_bank_by_num_noprobe(bank_nr);
	if (p)
	{
		if ((retval = p->driver->probe(p)) == ERROR_OK)
		{
			command_print(CMD_CTX, "flash '%s' found at 0x%8.8" PRIx32, p->driver->name, p->base);
		}
		else if (retval == ERROR_FLASH_BANK_INVALID)
		{
			command_print(CMD_CTX, "probing failed for flash bank '#%s' at 0x%8.8" PRIx32,
						  CMD_ARGV[0], p->base);
		}
		else
		{
			command_print(CMD_CTX, "unknown error when probing flash bank '#%s' at 0x%8.8" PRIx32,
						  CMD_ARGV[0], p->base);
		}
	}
	else
	{
		command_print(CMD_CTX, "flash bank '#%s' is out of bounds", CMD_ARGV[0]);
	}

	return ERROR_OK;
}

COMMAND_HANDLER(handle_flash_erase_check_command)
{
	if (CMD_ARGC != 1)
	{
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	struct flash_bank *p;
	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &p);
	if (ERROR_OK != retval)
		return retval;

	int j;
	if ((retval = p->driver->erase_check(p)) == ERROR_OK)
	{
		command_print(CMD_CTX, "successfully checked erase state");
	}
	else
	{
		command_print(CMD_CTX, "unknown error when checking erase state of flash bank #%s at 0x%8.8" PRIx32,
			CMD_ARGV[0], p->base);
	}

	for (j = 0; j < p->num_sectors; j++)
	{
		char *erase_state;

		if (p->sectors[j].is_erased == 0)
			erase_state = "not erased";
		else if (p->sectors[j].is_erased == 1)
			erase_state = "erased";
		else
			erase_state = "erase state unknown";

		command_print(CMD_CTX,
			      "\t#%3i: 0x%8.8" PRIx32 " (0x%" PRIx32 " %" PRIi32 "kB) %s",
			      j,
			      p->sectors[j].offset,
			      p->sectors[j].size,
			      p->sectors[j].size >> 10,
			      erase_state);
	}

	return ERROR_OK;
}

COMMAND_HANDLER(handle_flash_erase_address_command)
{
	struct flash_bank *p;
	int retval;
	int address;
	int length;

	struct target *target = get_current_target(CMD_CTX);

	if (CMD_ARGC != 2)
		return ERROR_COMMAND_SYNTAX_ERROR;

	COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], address);
	COMMAND_PARSE_NUMBER(int, CMD_ARGV[1], length);
	if (length <= 0)
	{
		command_print(CMD_CTX, "Length must be >0");
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	p = get_flash_bank_by_addr(target, address);
	if (p == NULL)
	{
		return ERROR_FAIL;
	}

	/* We can't know if we did a resume + halt, in which case we no longer know the erased state */
	flash_set_dirty();

	struct duration bench;
	duration_start(&bench);

	retval = flash_erase_address_range(target, address, length);

	if ((ERROR_OK == retval) && (duration_measure(&bench) == ERROR_OK))
	{
		command_print(CMD_CTX, "erased address 0x%8.8x (length %i)"
				" in %fs (%0.3f kb/s)", address, length,
				duration_elapsed(&bench), duration_kbps(&bench, length));
	}

	return retval;
}

COMMAND_HANDLER(handle_flash_protect_check_command)
{
	if (CMD_ARGC != 1)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct flash_bank *p;
	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &p);
	if (ERROR_OK != retval)
		return retval;

	if ((retval = p->driver->protect_check(p)) == ERROR_OK)
	{
		command_print(CMD_CTX, "successfully checked protect state");
	}
	else if (retval == ERROR_FLASH_OPERATION_FAILED)
	{
		command_print(CMD_CTX, "checking protection state failed (possibly unsupported) by flash #%s at 0x%8.8" PRIx32, CMD_ARGV[0], p->base);
	}
	else
	{
		command_print(CMD_CTX, "unknown error when checking protection state of flash bank '#%s' at 0x%8.8" PRIx32, CMD_ARGV[0], p->base);
	}

	return ERROR_OK;
}

static int flash_check_sector_parameters(struct command_context *cmd_ctx,
		uint32_t first, uint32_t last, uint32_t num_sectors)
{
	if (!(first <= last)) {
		command_print(cmd_ctx, "ERROR: "
				"first sector must be <= last sector");
		return ERROR_FAIL;
	}

	if (!(last <= (num_sectors - 1))) {
		command_print(cmd_ctx, "ERROR: last sector must be <= %d",
				(int) num_sectors - 1);
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

COMMAND_HANDLER(handle_flash_erase_command)
{
	if (CMD_ARGC != 3)
		return ERROR_COMMAND_SYNTAX_ERROR;

	uint32_t bank_nr;
	uint32_t first;
	uint32_t last;

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], bank_nr);
	struct flash_bank *p = get_flash_bank_by_num(bank_nr);
	if (!p)
		return ERROR_OK;

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], first);
	if (strcmp(CMD_ARGV[2], "last") == 0)
		last = p->num_sectors - 1;
	else
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[2], last);

	int retval;
	if ((retval = flash_check_sector_parameters(CMD_CTX,
			first, last, p->num_sectors)) != ERROR_OK)
		return retval;

	struct duration bench;
	duration_start(&bench);

	retval = flash_driver_erase(p, first, last);

	if ((ERROR_OK == retval) && (duration_measure(&bench) == ERROR_OK))
	{
		command_print(CMD_CTX, "erased sectors %" PRIu32 " "
				"through %" PRIu32" on flash bank %" PRIu32 " "
				"in %fs", first, last, bank_nr, duration_elapsed(&bench));
	}

	return ERROR_OK;
}

COMMAND_HANDLER(handle_flash_protect_command)
{
	if (CMD_ARGC != 4)
		return ERROR_COMMAND_SYNTAX_ERROR;

	uint32_t bank_nr;
	uint32_t first;
	uint32_t last;

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], bank_nr);
	struct flash_bank *p = get_flash_bank_by_num(bank_nr);
	if (!p)
		return ERROR_OK;

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], first);
	if (strcmp(CMD_ARGV[2], "last") == 0)
		last = p->num_sectors - 1;
	else
		COMMAND_PARSE_NUMBER(u32, CMD_ARGV[2], last);

	bool set;
	COMMAND_PARSE_ON_OFF(CMD_ARGV[3], set);

	int retval;
	if ((retval = flash_check_sector_parameters(CMD_CTX,
			first, last, p->num_sectors)) != ERROR_OK)
		return retval;

	retval = flash_driver_protect(p, set, first, last);
	if (retval == ERROR_OK) {
		command_print(CMD_CTX, "%s protection for sectors %i "
				"through %i on flash bank %i",
			(set) ? "set" : "cleared", (int) first,
			(int) last, (int) bank_nr);
	}

	return ERROR_OK;
}

COMMAND_HANDLER(handle_flash_write_image_command)
{
	struct target *target = get_current_target(CMD_CTX);

	struct image image;
	uint32_t written;

	int retval;

	if (CMD_ARGC < 1)
	{
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	/* flash auto-erase is disabled by default*/
	int auto_erase = 0;
	bool auto_unlock = false;

	for (;;)
	{
		if (strcmp(CMD_ARGV[0], "erase") == 0)
		{
			auto_erase = 1;
			CMD_ARGV++;
			CMD_ARGC--;
			command_print(CMD_CTX, "auto erase enabled");
		} else if (strcmp(CMD_ARGV[0], "unlock") == 0)
		{
			auto_unlock = true;
			CMD_ARGV++;
			CMD_ARGC--;
			command_print(CMD_CTX, "auto unlock enabled");
		} else
		{
			break;
		}
	}

	if (CMD_ARGC < 1)
	{
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	if (!target)
	{
		LOG_ERROR("no target selected");
		return ERROR_FAIL;
	}

	struct duration bench;
	duration_start(&bench);

	if (CMD_ARGC >= 2)
	{
		image.base_address_set = 1;
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[1], image.base_address);
	}
	else
	{
		image.base_address_set = 0;
		image.base_address = 0x0;
	}

	image.start_address_set = 0;

	retval = image_open(&image, CMD_ARGV[0], (CMD_ARGC == 3) ? CMD_ARGV[2] : NULL);
	if (retval != ERROR_OK)
	{
		return retval;
	}

	retval = flash_write_unlock(target, &image, &written, auto_erase, auto_unlock);
	if (retval != ERROR_OK)
	{
		image_close(&image);
		return retval;
	}

	if ((ERROR_OK == retval) && (duration_measure(&bench) == ERROR_OK))
	{
		command_print(CMD_CTX, "wrote %" PRIu32 " byte from file %s "
				"in %fs (%0.3f kb/s)", written, CMD_ARGV[0],
				duration_elapsed(&bench), duration_kbps(&bench, written));
	}

	image_close(&image);

	return retval;
}

COMMAND_HANDLER(handle_flash_fill_command)
{
	int err = ERROR_OK;
	uint32_t address;
	uint32_t pattern;
	uint32_t count;
	uint32_t wrote = 0;
	uint32_t cur_size = 0;
	uint32_t chunk_count;
	struct target *target = get_current_target(CMD_CTX);
	uint32_t i;
	uint32_t wordsize;
	int retval = ERROR_OK;

	static size_t const chunksize = 1024;
	uint8_t *chunk = malloc(chunksize);
	if (chunk == NULL)
		return ERROR_FAIL;

	uint8_t *readback = malloc(chunksize);
	if (readback == NULL)
	{
		free(chunk);
		return ERROR_FAIL;
	}


	if (CMD_ARGC != 3)
	{
		retval = ERROR_COMMAND_SYNTAX_ERROR;
		goto done;
	}


	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[0], address);
	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[1], pattern);
	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[2], count);

	if (count == 0)
		goto done;

	switch (CMD_NAME[4])
	{
	case 'w':
		wordsize = 4;
		break;
	case 'h':
		wordsize = 2;
		break;
	case 'b':
		wordsize = 1;
		break;
	default:
		retval = ERROR_COMMAND_SYNTAX_ERROR;
		goto done;
	}

	chunk_count = MIN(count, (chunksize / wordsize));
	switch (wordsize)
	{
	case 4:
		for (i = 0; i < chunk_count; i++)
		{
			target_buffer_set_u32(target, chunk + i * wordsize, pattern);
		}
		break;
	case 2:
		for (i = 0; i < chunk_count; i++)
		{
			target_buffer_set_u16(target, chunk + i * wordsize, pattern);
		}
		break;
	case 1:
		memset(chunk, pattern, chunk_count);
		break;
	default:
		LOG_ERROR("BUG: can't happen");
		exit(-1);
	}

	struct duration bench;
	duration_start(&bench);

	for (wrote = 0; wrote < (count*wordsize); wrote += cur_size)
	{
		cur_size = MIN((count*wordsize - wrote), sizeof(chunk));
		struct flash_bank *bank;
		bank = get_flash_bank_by_addr(target, address);
		if (bank == NULL)
		{
			retval = ERROR_FAIL;
			goto done;
		}
		err = flash_driver_write(bank, chunk, address - bank->base + wrote, cur_size);
		if (err != ERROR_OK)
		{
			retval = err;
			goto done;
		}

		err = target_read_buffer(target, address + wrote, cur_size, readback);
		if (err != ERROR_OK)
		{
			retval = err;
			goto done;
		}

		unsigned i;
		for (i = 0; i < cur_size; i++)
		{
			if (readback[i]!=chunk[i])
			{
				LOG_ERROR("Verfication error address 0x%08" PRIx32 ", read back 0x%02x, expected 0x%02x",
						  address + wrote + i, readback[i], chunk[i]);
				retval = ERROR_FAIL;
				goto done;
			}
		}
	}

	if (duration_measure(&bench) == ERROR_OK)
	{
		command_print(CMD_CTX, "wrote %" PRIu32 " bytes to 0x%8.8" PRIx32
				" in %fs (%0.3f kb/s)", wrote, address,
				duration_elapsed(&bench), duration_kbps(&bench, wrote));
	}

	done:
	free(readback);
	free(chunk);

	return retval;
}

COMMAND_HANDLER(handle_flash_write_bank_command)
{
	uint32_t offset;
	uint8_t *buffer;
	struct fileio fileio;

	if (CMD_ARGC != 3)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct duration bench;
	duration_start(&bench);

	struct flash_bank *p;
	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &p);
	if (ERROR_OK != retval)
		return retval;

	COMMAND_PARSE_NUMBER(u32, CMD_ARGV[2], offset);

	if (fileio_open(&fileio, CMD_ARGV[1], FILEIO_READ, FILEIO_BINARY) != ERROR_OK)
	{
		return ERROR_OK;
	}

	buffer = malloc(fileio.size);
	size_t buf_cnt;
	if (fileio_read(&fileio, fileio.size, buffer, &buf_cnt) != ERROR_OK)
	{
		free(buffer);
		fileio_close(&fileio);
		return ERROR_OK;
	}

	retval = flash_driver_write(p, buffer, offset, buf_cnt);

	free(buffer);
	buffer = NULL;

	if ((ERROR_OK == retval) && (duration_measure(&bench) == ERROR_OK))
	{
		command_print(CMD_CTX, "wrote %zu byte from file %s to flash bank %u"
				" at offset 0x%8.8" PRIx32 " in %fs (%0.3f kb/s)",
				fileio.size, CMD_ARGV[1], p->bank_number, offset,
				duration_elapsed(&bench), duration_kbps(&bench, fileio.size));
	}

	fileio_close(&fileio);

	return retval;
}

void flash_set_dirty(void)
{
	struct flash_bank *c;
	int i;

	/* set all flash to require erasing */
	for (c = flash_banks; c; c = c->next)
	{
		for (i = 0; i < c->num_sectors; i++)
		{
			c->sectors[i].is_erased = 0;
		}
	}
}

/* lookup flash bank by address */
struct flash_bank *get_flash_bank_by_addr(struct target *target, uint32_t addr)
{
	struct flash_bank *c;

	/* cycle through bank list */
	for (c = flash_banks; c; c = c->next)
	{
		int retval;
		retval = c->driver->auto_probe(c);

		if (retval != ERROR_OK)
		{
			LOG_ERROR("auto_probe failed %d\n", retval);
			return NULL;
		}
		/* check whether address belongs to this flash bank */
		if ((addr >= c->base) && (addr <= c->base + (c->size - 1)) && target == c->target)
			return c;
	}
	LOG_ERROR("No flash at address 0x%08" PRIx32 "\n", addr);
	return NULL;
}

/* erase given flash region, selects proper bank according to target and address */
static int flash_iterate_address_range(struct target *target, uint32_t addr, uint32_t length,
		int (*callback)(struct flash_bank *bank, int first, int last))
{
	struct flash_bank *c;
	int first = -1;
	int last = -1;
	int i;

	if ((c = get_flash_bank_by_addr(target, addr)) == NULL)
		return ERROR_FLASH_DST_OUT_OF_BANK; /* no corresponding bank found */

	if (c->size == 0 || c->num_sectors == 0)
	{
		LOG_ERROR("Bank is invalid");
		return ERROR_FLASH_BANK_INVALID;
	}

	if (length == 0)
	{
		/* special case, erase whole bank when length is zero */
		if (addr != c->base)
			return ERROR_FLASH_DST_BREAKS_ALIGNMENT;

		return callback(c, 0, c->num_sectors - 1);
	}

	/* check whether it fits */
	if (addr + length - 1 > c->base + c->size - 1)
		return ERROR_FLASH_DST_BREAKS_ALIGNMENT;

	addr -= c->base;

	for (i = 0; i < c->num_sectors; i++)
	{
		/* check whether sector overlaps with the given range and is not yet erased */
		if (addr < c->sectors[i].offset + c->sectors[i].size && addr + length > c->sectors[i].offset && c->sectors[i].is_erased != 1) {
			/* if first is not set yet then this is the first sector */
			if (first == -1)
				first = i;
			last = i; /* and it is the last one so far in any case */
		}
	}

	if (first == -1 || last == -1)
		return ERROR_OK;

	return callback(c, first, last);
}



int flash_erase_address_range(struct target *target, uint32_t addr, uint32_t length)
{
	return flash_iterate_address_range(target, addr, length, &flash_driver_erase);
}

static int flash_driver_unprotect(struct flash_bank *bank, int first, int last)
{
	return flash_driver_protect(bank, 0, first, last);
}

static int flash_unlock_address_range(struct target *target, uint32_t addr, uint32_t length)
{
	return flash_iterate_address_range(target, addr, length, &flash_driver_unprotect);
}


/* write (optional verify) an image to flash memory of the given target */
static int flash_write_unlock(struct target *target, struct image *image, uint32_t *written, int erase, bool unlock)
{
	int retval = ERROR_OK;

	int section;
	uint32_t section_offset;
	struct flash_bank *c;
	int *padding;

	section = 0;
	section_offset = 0;

	if (written)
		*written = 0;

	if (erase)
	{
		/* assume all sectors need erasing - stops any problems
		 * when flash_write is called multiple times */

		flash_set_dirty();
	}

	/* allocate padding array */
	padding = malloc(image->num_sections * sizeof(padding));

	/* loop until we reach end of the image */
	while (section < image->num_sections)
	{
		uint32_t buffer_size;
		uint8_t *buffer;
		int section_first;
		int section_last;
		uint32_t run_address = image->sections[section].base_address + section_offset;
		uint32_t run_size = image->sections[section].size - section_offset;
		int pad_bytes = 0;

		if (image->sections[section].size ==  0)
		{
			LOG_WARNING("empty section %d", section);
			section++;
			section_offset = 0;
			continue;
		}

		/* find the corresponding flash bank */
		if ((c = get_flash_bank_by_addr(target, run_address)) == NULL)
		{
			section++; /* and skip it */
			section_offset = 0;
			continue;
		}

		/* collect consecutive sections which fall into the same bank */
		section_first = section;
		section_last = section;
		padding[section] = 0;
		while ((run_address + run_size - 1 < c->base + c->size - 1)
				&& (section_last + 1 < image->num_sections))
		{
			if (image->sections[section_last + 1].base_address < (run_address + run_size))
			{
				LOG_DEBUG("section %d out of order(very slightly surprising, but supported)", section_last + 1);
				break;
			}
			/* if we have multiple sections within our image, flash programming could fail due to alignment issues
			 * attempt to rebuild a consecutive buffer for the flash loader */
			pad_bytes = (image->sections[section_last + 1].base_address) - (run_address + run_size);
			if ((run_address + run_size + pad_bytes) > (c->base + c->size))
				break;
			padding[section_last] = pad_bytes;
			run_size += image->sections[++section_last].size;
			run_size += pad_bytes;
			padding[section_last] = 0;

			LOG_INFO("Padding image section %d with %d bytes", section_last-1, pad_bytes);
		}

		/* fit the run into bank constraints */
		if (run_address + run_size - 1 > c->base + c->size - 1)
		{
			LOG_WARNING("writing %d bytes only - as image section is %d bytes and bank is only %d bytes", \
				    (int)(c->base + c->size - run_address), (int)(run_size), (int)(c->size));
			run_size = c->base + c->size - run_address;
		}

		/* allocate buffer */
		buffer = malloc(run_size);
		buffer_size = 0;

		/* read sections to the buffer */
		while (buffer_size < run_size)
		{
			size_t size_read;

			size_read = run_size - buffer_size;
			if (size_read > image->sections[section].size - section_offset)
			    size_read = image->sections[section].size - section_offset;

			if ((retval = image_read_section(image, section, section_offset,
					size_read, buffer + buffer_size, &size_read)) != ERROR_OK || size_read == 0)
			{
				free(buffer);
				free(padding);
				return retval;
			}

			/* see if we need to pad the section */
			while (padding[section]--)
				 (buffer + buffer_size)[size_read++] = 0xff;

			buffer_size += size_read;
			section_offset += size_read;

			if (section_offset >= image->sections[section].size)
			{
				section++;
				section_offset = 0;
			}
		}

		retval = ERROR_OK;

		if (unlock)
		{
			retval = flash_unlock_address_range(target, run_address, run_size);
		}
		if (retval == ERROR_OK)
		{
			if (erase)
			{
				/* calculate and erase sectors */
				retval = flash_erase_address_range(target, run_address, run_size);
			}
		}

		if (retval == ERROR_OK)
		{
			/* write flash sectors */
			retval = flash_driver_write(c, buffer, run_address - c->base, run_size);
		}

		free(buffer);

		if (retval != ERROR_OK)
		{
			free(padding);
			return retval; /* abort operation */
		}

		if (written != NULL)
			*written += run_size; /* add run size to total written counter */
	}

	free(padding);

	return retval;
}

int flash_write(struct target *target, struct image *image, uint32_t *written, int erase)
{
	return flash_write_unlock(target, image, written, erase, false);
}

int default_flash_mem_blank_check(struct flash_bank *bank)
{
	struct target *target = bank->target;
	const int buffer_size = 1024;
	int i;
	uint32_t nBytes;
	int retval = ERROR_OK;

	if (bank->target->state != TARGET_HALTED)
	{
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	uint8_t *buffer = malloc(buffer_size);

	for (i = 0; i < bank->num_sectors; i++)
	{
		uint32_t j;
		bank->sectors[i].is_erased = 1;

		for (j = 0; j < bank->sectors[i].size; j += buffer_size)
		{
			uint32_t chunk;
			chunk = buffer_size;
			if (chunk > (j - bank->sectors[i].size))
			{
				chunk = (j - bank->sectors[i].size);
			}

			retval = target_read_memory(target, bank->base + bank->sectors[i].offset + j, 4, chunk/4, buffer);
			if (retval != ERROR_OK)
			{
				goto done;
			}

			for (nBytes = 0; nBytes < chunk; nBytes++)
			{
				if (buffer[nBytes] != 0xFF)
				{
					bank->sectors[i].is_erased = 0;
					break;
				}
			}
		}
	}

	done:
	free(buffer);

	return retval;
}

int default_flash_blank_check(struct flash_bank *bank)
{
	struct target *target = bank->target;
	int i;
	int retval;
	int fast_check = 0;
	uint32_t blank;

	if (bank->target->state != TARGET_HALTED)
	{
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	for (i = 0; i < bank->num_sectors; i++)
	{
		uint32_t address = bank->base + bank->sectors[i].offset;
		uint32_t size = bank->sectors[i].size;

		if ((retval = target_blank_check_memory(target, address, size, &blank)) != ERROR_OK)
		{
			fast_check = 0;
			break;
		}
		if (blank == 0xFF)
			bank->sectors[i].is_erased = 1;
		else
			bank->sectors[i].is_erased = 0;
		fast_check = 1;
	}

	if (!fast_check)
	{
		LOG_USER("Running slow fallback erase check - add working memory");
		return default_flash_mem_blank_check(bank);
	}

	return ERROR_OK;
}

static const struct command_registration flash_exec_command_handlers[] = {
	{
		.name = "probe",
		.handler = &handle_flash_probe_command,
		.mode = COMMAND_EXEC,
		.usage = "<bank>",
		.help = "identify flash bank",
	},
	{
		.name = "info",
		.handler = &handle_flash_info_command,
		.mode = COMMAND_EXEC,
		.usage = "<bank>",
		.help = "print bank information",
	},
	{
		.name = "erase_check",
		.handler = &handle_flash_erase_check_command,
		.mode = COMMAND_EXEC,
		.usage = "<bank>",
		.help = "check erase state of sectors",
	},
	{
		.name = "protect_check",
		.handler = &handle_flash_protect_check_command,
		.mode = COMMAND_EXEC,
		.usage = "<bank>",
		.help = "check protection state of sectors",
	},
	{
		.name = "erase_sector",
		.handler = &handle_flash_erase_command,
		.mode = COMMAND_EXEC,
		.usage = "<bank> <first> <last>",
		.help = "erase sectors",
	},
	{
		.name = "erase_address",
		.handler = &handle_flash_erase_address_command,
		.mode = COMMAND_EXEC,
		.usage = "<address> <length>",
		.help = "erase address range",

	},
	{
		.name = "fillw",
		.handler = &handle_flash_fill_command,
		.mode = COMMAND_EXEC,
		.usage = "<bank> <address> <word_pattern> <count>",
		.help = "fill with pattern (no autoerase)",
	},
	{
		.name = "fillh",
		.handler = &handle_flash_fill_command,
		.mode = COMMAND_EXEC,
		.usage = "<bank> <address> <halfword_pattern> <count>",
		.help = "fill with pattern",
	},
	{
		.name = "fillb",
		.handler = &handle_flash_fill_command,
		.mode = COMMAND_EXEC,
		.usage = "<bank> <address> <byte_pattern> <count>",
		.help = "fill with pattern",

	},
	{
		.name = "write_bank",
		.handler = &handle_flash_write_bank_command,
		.mode = COMMAND_EXEC,
		.usage = "<bank> <file> <offset>",
		.help = "write binary data",
	},
	{
		.name = "write_image",
		.handler = &handle_flash_write_image_command,
		.mode = COMMAND_EXEC,
		.usage = "<bank> [erase] [unlock] <file> [offset] [type]",
		.help = "write an image to flash"
	},
	{
		.name = "protect",
		.handler = &handle_flash_protect_command,
		.mode = COMMAND_EXEC,
		.usage = "<bank> <first> <last> <on | off>",
		.help = "set protection of sectors",
	},
	COMMAND_REGISTRATION_DONE
};

int flash_init_drivers(struct command_context *cmd_ctx)
{
	if (!flash_banks)
		return ERROR_OK;

	struct command *parent = command_find_in_context(cmd_ctx, "flash");
	return register_commands(cmd_ctx, parent, flash_exec_command_handlers);
}

COMMAND_HANDLER(handle_flash_init_command)
{
	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	static bool flash_initialized = false;
	if (flash_initialized)
	{
		LOG_INFO("'flash init' has already been called");
		return ERROR_OK;
	}
	flash_initialized = true;

	LOG_DEBUG("Initializing flash devices...");
	return flash_init_drivers(CMD_CTX);
}

static const struct command_registration flash_config_command_handlers[] = {
	{
		.name = "bank",
		.handler = &handle_flash_bank_command,
		.mode = COMMAND_CONFIG,
		.usage = "<name> <driver> <base> <size> "
			"<chip_width> <bus_width> <target> "
			"[driver_options ...]",
		.help = "Define a new bank with the given name, "
			"using the specified NOR flash driver.",
	},
	{
		.name = "init",
		.mode = COMMAND_CONFIG,
		.handler = &handle_flash_init_command,
		.help = "initialize flash devices",
	},
	{
		.name = "banks",
		.mode = COMMAND_ANY,
		.jim_handler = &jim_flash_banks,
		.help = "return information about the flash banks",
	},
	COMMAND_REGISTRATION_DONE
};
static const struct command_registration flash_command_handlers[] = {
	{
		.name = "flash",
		.mode = COMMAND_ANY,
		.help = "NOR flash command group",
		.chain = flash_config_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

int flash_register_commands(struct command_context *cmd_ctx)
{
	return register_commands(cmd_ctx, NULL, flash_command_handlers);
}
