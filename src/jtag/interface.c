/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2007,2008 �yvind Harboe                                 *
 *   oyvind.harboe@zylin.com                                               *
 *                                                                         *
 *   Copyright (C) 2009 SoftPLC Corporation                                *
 *       http://softplc.com                                                *
 *   dick@softplc.com                                                      *
 *                                                                         *
 *   Copyright (C) 2009 Zachary T Welch                                    *
 *   zw@superlucidity.net                                                  *
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

#include "jtag.h"
#include "interface.h"

/**
 * @see tap_set_state() and tap_get_state() accessors.
 * Actual name is not important since accessors hide it.
 */
static tap_state_t state_follower = TAP_RESET;

void tap_set_state_impl( tap_state_t new_state )
{
	/* this is the state we think the TAPs are in now, was cur_state */
	state_follower = new_state;
}

tap_state_t tap_get_state()
{
	return state_follower;
}

/**
 * @see tap_set_end_state() and tap_get_end_state() accessors.
 * Actual name is not important because accessors hide it.
 */
static tap_state_t end_state_follower = TAP_RESET;

void tap_set_end_state( tap_state_t new_end_state )
{
	/* this is the state we think the TAPs will be in at completion of the
	   current TAP operation, was end_state
	*/
	end_state_follower = new_end_state;
}

tap_state_t tap_get_end_state()
{
	return end_state_follower;
}


int tap_move_ndx( tap_state_t astate )
{
	/* given a stable state, return the index into the tms_seqs[] array within tap_get_tms_path() */

	int ndx;

	switch( astate )
	{
	case TAP_RESET:		ndx = 0;			break;
	case TAP_DRSHIFT:	ndx = 2;			break;
	case TAP_DRPAUSE:	ndx = 3;			break;
	case TAP_IDLE:		ndx = 1;			break;
	case TAP_IRSHIFT:	ndx = 4;			break;
	case TAP_IRPAUSE:	ndx = 5;			break;
	default:
		LOG_ERROR( "fatal: unstable state \"%s\" used in tap_move_ndx()", tap_state_name(astate) );
		exit(1);
	}

	return ndx;
}


/* tap_move[i][j]: tap movement command to go from state i to state j
 * 0: Test-Logic-Reset
 * 1: Run-Test/Idle
 * 2: Shift-DR
 * 3: Pause-DR
 * 4: Shift-IR
 * 5: Pause-IR
 *
 * DRSHIFT->DRSHIFT and IRSHIFT->IRSHIFT have to be caught in interface specific code
 */
struct tms_sequences
{
	u8	bits;
	u8	bit_count;

};

/*
 * These macros allow us to specify TMS state transitions by bits rather than hex bytes.
 * Read the bits from LSBit first to MSBit last (right-to-left).
 */
#define HEX__(n) 0x##n##LU

#define B8__(x) \
	 (((x) & 0x0000000FLU)?(1<<0):0) \
	+(((x) & 0x000000F0LU)?(1<<1):0) \
	+(((x) & 0x00000F00LU)?(1<<2):0) \
	+(((x) & 0x0000F000LU)?(1<<3):0) \
	+(((x) & 0x000F0000LU)?(1<<4):0) \
	+(((x) & 0x00F00000LU)?(1<<5):0) \
	+(((x) & 0x0F000000LU)?(1<<6):0) \
	+(((x) & 0xF0000000LU)?(1<<7):0)

#define B8(bits,count)		{ ((u8)B8__(HEX__(bits))), (count) }

static const struct tms_sequences old_tms_seqs[6][6] =		/*  [from_state_ndx][to_state_ndx] */
{
	/* value clocked to TMS to move from one of six stable states to another.
	 * N.B. OOCD clocks TMS from LSB first, so read these right-to-left.
	 * N.B. These values are tightly bound to the table in tap_get_tms_path_len().
	 * N.B. Reset only needs to be 0b11111, but in JLink an even byte of 1's is more stable.
	 * These extra ones cause no TAP state problem, because we go into reset and stay in reset.
	 */



	/* to state: */
	/*	RESET			IDLE			DRSHIFT			DRPAUSE			IRSHIFT			IRPAUSE 	*/		/* from state: */
	{	B8(1111111,7),	B8(0000000,7),	B8(0010111,7),	B8(0001010,7),	B8(0011011,7),	B8(0010110,7) },	/* RESET */
	{	B8(1111111,7),	B8(0000000,7),	B8(0100101,7),	B8(0000101,7),	B8(0101011,7),	B8(0001011,7) },	/* IDLE */
	{	B8(1111111,7),	B8(0110001,7),	B8(0000000,7),	B8(0000001,7),	B8(0001111,7),	B8(0101111,7) },	/* DRSHIFT */
	{	B8(1111111,7),	B8(0110000,7),	B8(0100000,7),	B8(0010111,7),	B8(0011110,7),	B8(0101111,7) },	/* DRPAUSE */
	{	B8(1111111,7),	B8(0110001,7),	B8(0000111,7),	B8(0010111,7),	B8(0000000,7),	B8(0000001,7) },	/* IRSHIFT */
	{	B8(1111111,7),	B8(0110000,7),	B8(0011100,7),	B8(0010111,7),	B8(0011110,7),	B8(0101111,7) },	/* IRPAUSE */
};



static const struct tms_sequences short_tms_seqs[6][6] =		/*  [from_state_ndx][to_state_ndx] */
{
	/* this is the table submitted by Jeff Williams on 3/30/2009 with this comment:

		OK, I added Peter's version of the state table, and it works OK for
		me on MC1322x. I've recreated the jlink portion of patch with this
		new state table. His changes to my state table are pretty minor in
		terms of total transitions, but Peter feels that his version fixes
		some long-standing problems.
		Jeff

		I added the bit count into the table, reduced RESET column to 7 bits from 8.
		Dick

		state specific comments:
		------------------------
		*->RESET		   tried the 5 bit reset and it gave me problems, 7 bits seems to
					   work better on ARM9 with ft2232 driver.  (Dick)

		RESET->DRSHIFT add 1 extra clock cycles in the RESET state before advancing.
						needed on ARM9 with ft2232 driver.  (Dick)

		RESET->IRSHIFT add 1 extra clock cycles in the RESET state before advancing.
						needed on ARM9 with ft2232 driver.  (Dick)
	*/

	/* to state: */
	/*	RESET			IDLE				DRSHIFT			DRPAUSE			IRSHIFT	  		IRPAUSE */			/* from state: */
	{	B8(1111111,7),	B8(0000000,7),	B8(0010111,7),		B8(0001010,7),	B8(0011011,7),	B8(0010110,7) },	/* RESET */
	{	B8(1111111,7),	B8(0000000,7),	B8(001,3),			B8(0101,4),		B8(0011,4),  	B8(01011,5) },		/* IDLE */
	{	B8(1111111,7),	B8(011,3),		B8(00111,5),		B8(01,2),		B8(001111,6),	B8(0101111,7) },	/* DRSHIFT */
	{	B8(1111111,7),	B8(011,3),		B8(01,2),   		B8(0,1),		B8(001111,6),	B8(0101111,7) },	/* DRPAUSE */
	{	B8(1111111,7),	B8(011,3),		B8(00111,5),		B8(010111,6), 	B8(001111,6),	B8(01,2) },			/* IRSHIFT */
	{	B8(1111111,7),	B8(011,3),		B8(00111,5),		B8(010111,6),	B8(01,2),		B8(0,1) } 			/* IRPAUSE */

};

typedef const struct tms_sequences tms_table[6][6];

static tms_table *tms_seqs=&short_tms_seqs;

int tap_get_tms_path( tap_state_t from, tap_state_t to )
{
	return (*tms_seqs)[tap_move_ndx(from)][tap_move_ndx(to)].bits;
}


int tap_get_tms_path_len( tap_state_t from, tap_state_t to )
{
	return (*tms_seqs)[tap_move_ndx(from)][tap_move_ndx(to)].bit_count;
}


bool tap_is_state_stable(tap_state_t astate)
{
	bool is_stable;

	/*	A switch() is used because it is symbol dependent
		(not value dependent like an array), and can also check bounds.
	*/
	switch( astate )
	{
	case TAP_RESET:
	case TAP_IDLE:
	case TAP_DRSHIFT:
	case TAP_DRPAUSE:
	case TAP_IRSHIFT:
	case TAP_IRPAUSE:
		is_stable = true;
		break;
	default:
		is_stable = false;
	}

	return is_stable;
}

tap_state_t tap_state_transition(tap_state_t cur_state, bool tms)
{
	tap_state_t new_state;

	/*	A switch is used because it is symbol dependent and not value dependent
		like an array.  Also it can check for out of range conditions.
	*/

	if (tms)
	{
		switch (cur_state)
		{
		case TAP_RESET:
			new_state = cur_state;
			break;
		case TAP_IDLE:
		case TAP_DRUPDATE:
		case TAP_IRUPDATE:
			new_state = TAP_DRSELECT;
			break;
		case TAP_DRSELECT:
			new_state = TAP_IRSELECT;
			break;
		case TAP_DRCAPTURE:
		case TAP_DRSHIFT:
			new_state = TAP_DREXIT1;
			break;
		case TAP_DREXIT1:
		case TAP_DREXIT2:
			new_state = TAP_DRUPDATE;
			break;
		case TAP_DRPAUSE:
			new_state = TAP_DREXIT2;
			break;
		case TAP_IRSELECT:
			new_state = TAP_RESET;
			break;
		case TAP_IRCAPTURE:
		case TAP_IRSHIFT:
			new_state = TAP_IREXIT1;
			break;
		case TAP_IREXIT1:
		case TAP_IREXIT2:
			new_state = TAP_IRUPDATE;
			break;
		case TAP_IRPAUSE:
			new_state = TAP_IREXIT2;
			break;
		default:
			LOG_ERROR( "fatal: invalid argument cur_state=%d", cur_state );
			exit(1);
			break;
		}
	}
	else
	{
		switch (cur_state)
		{
		case TAP_RESET:
		case TAP_IDLE:
		case TAP_DRUPDATE:
		case TAP_IRUPDATE:
			new_state = TAP_IDLE;
			break;
		case TAP_DRSELECT:
			new_state = TAP_DRCAPTURE;
			break;
		case TAP_DRCAPTURE:
		case TAP_DRSHIFT:
		case TAP_DREXIT2:
			new_state = TAP_DRSHIFT;
			break;
		case TAP_DREXIT1:
		case TAP_DRPAUSE:
			new_state = TAP_DRPAUSE;
			break;
		case TAP_IRSELECT:
			new_state = TAP_IRCAPTURE;
			break;
		case TAP_IRCAPTURE:
		case TAP_IRSHIFT:
		case TAP_IREXIT2:
			new_state = TAP_IRSHIFT;
			break;
		case TAP_IREXIT1:
		case TAP_IRPAUSE:
			new_state = TAP_IRPAUSE;
			break;
		default:
			LOG_ERROR( "fatal: invalid argument cur_state=%d", cur_state );
			exit(1);
			break;
		}
	}

	return new_state;
}

const char* tap_state_name(tap_state_t state)
{
	const char* ret;

	switch( state )
	{
	case TAP_RESET:		ret = "RESET";			break;
	case TAP_IDLE:		ret = "RUN/IDLE";		break;
	case TAP_DRSELECT:	ret = "DRSELECT";		break;
	case TAP_DRCAPTURE: ret = "DRCAPTURE";		break;
	case TAP_DRSHIFT:	ret = "DRSHIFT";			break;
	case TAP_DREXIT1:	ret = "DREXIT1";			break;
	case TAP_DRPAUSE:	ret = "DRPAUSE";			break;
	case TAP_DREXIT2:	ret = "DREXIT2";			break;
	case TAP_DRUPDATE:	ret = "DRUPDATE";		break;
	case TAP_IRSELECT:	ret = "IRSELECT";		break;
	case TAP_IRCAPTURE: ret = "IRCAPTURE";		break;
	case TAP_IRSHIFT:	ret = "IRSHIFT";			break;
	case TAP_IREXIT1:	ret = "IREXIT1";			break;
	case TAP_IRPAUSE:	ret = "IRPAUSE";			break;
	case TAP_IREXIT2:	ret = "IREXIT2";			break;
	case TAP_IRUPDATE:	ret = "IRUPDATE";		break;
	default:				ret = "???";
	}

	return ret;
}

tap_state_t tap_state_by_name(const char *name)
{
	tap_state_t x;

	for( x = 0 ; x < TAP_NUM_STATES ; x++ ){
		/* be nice to the human */
		if( 0 == strcasecmp( name, tap_state_name(x) ) ){
			return x;
		}
	}
	/* not found */
	return TAP_INVALID;
}

#ifdef _DEBUG_JTAG_IO_

#define JTAG_DEBUG_STATE_APPEND(buf, len, bit) \
		do { buf[len] = bit ? '1' : '0'; } while(0)
#define JTAG_DEBUG_STATE_PRINT(a, b, astr, bstr) \
		DEBUG_JTAG_IO("TAP/SM: %9s -> %5s\tTMS: %s\tTDI: %s", \
			tap_state_name(a), tap_state_name(b), astr, bstr)

tap_state_t jtag_debug_state_machine(const void *tms_buf, const void *tdi_buf,
		unsigned tap_bits, tap_state_t next_state)
{
	const u8 *tms_buffer;
	const u8 *tdi_buffer;
	unsigned tap_bytes;
	unsigned cur_byte;
	unsigned cur_bit;

	unsigned tap_out_bits;
	char tms_str[33];
	char tdi_str[33];

	tap_state_t last_state;

	// set startstate (and possibly last, if tap_bits == 0)
	last_state = next_state;
	DEBUG_JTAG_IO("TAP/SM: START state: %s", tap_state_name(next_state));

	tms_buffer = (const u8 *)tms_buf;
	tdi_buffer = (const u8 *)tdi_buf;

	tap_bytes = TAP_SCAN_BYTES(tap_bits);
	DEBUG_JTAG_IO("TAP/SM: TMS bits: %u (bytes: %u)", tap_bits, tap_bytes);

	tap_out_bits = 0;
	for(cur_byte = 0; cur_byte < tap_bytes; cur_byte++)
	{
		for(cur_bit = 0; cur_bit < 8; cur_bit++)
		{
			// make sure we do not run off the end of the buffers
			unsigned tap_bit = cur_byte * 8 + cur_bit;
			if (tap_bit == tap_bits)
				break;

			// check and save TMS bit
			tap_bit = !!(tms_buffer[cur_byte] & (1 << cur_bit));
			JTAG_DEBUG_STATE_APPEND(tms_str, tap_out_bits, tap_bit);

			// use TMS bit to find the next TAP state
			next_state = tap_state_transition(last_state, tap_bit);

			// check and store TDI bit
			tap_bit = !!(tdi_buffer[cur_byte] & (1 << cur_bit));
			JTAG_DEBUG_STATE_APPEND(tdi_str, tap_out_bits, tap_bit);

			// increment TAP bits
			tap_out_bits++;

			// Only show TDO bits on state transitions, or
			// after some number of bits in the same state.
			if ((next_state == last_state) && (tap_out_bits < 32))
				continue;

			// terminate strings and display state transition
			tms_str[tap_out_bits] = tdi_str[tap_out_bits] = 0;
			JTAG_DEBUG_STATE_PRINT(last_state, next_state, tms_str, tdi_str);

			// reset state
			last_state = next_state;
			tap_out_bits = 0;
		}
	}

	if (tap_out_bits)
	{
		// terminate strings and display state transition
		tms_str[tap_out_bits] = tdi_str[tap_out_bits] = 0;
		JTAG_DEBUG_STATE_PRINT(last_state, next_state, tms_str, tdi_str);
	}

	DEBUG_JTAG_IO("TAP/SM: FINAL state: %s", tap_state_name(next_state));

	return next_state;
}
#endif // _DEBUG_JTAG_IO_

void tap_use_new_tms_table(bool use_new)
{
	tms_seqs = use_new ? &short_tms_seqs : &old_tms_seqs;
}
bool tap_uses_new_tms_table(void)
{
	return tms_seqs == &short_tms_seqs;
}
