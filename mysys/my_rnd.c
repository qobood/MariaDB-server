/* Copyright (C) 2007 MySQL AB & Michael Widenius

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include "mysys_priv.h"
#include <my_rnd.h>
#include <m_string.h>

/*
  Initialize random generator

  NOTES
    MySQL's password checks depends on this, so don't do any changes
    that changes the random numbers that are generated!
*/

void my_rnd_init(struct my_rnd_struct *rand_st, ulong seed1, ulong seed2)
{
#ifdef HAVE_valgrind
  bzero((char*) rand_st,sizeof(*rand_st));      /* Avoid UMC warnings */
#endif
  rand_st->max_value= 0x3FFFFFFFL;
  rand_st->max_value_dbl=(double) rand_st->max_value;
  rand_st->seed1=seed1%rand_st->max_value ;
  rand_st->seed2=seed2%rand_st->max_value;
}


/*
  Generate random number.

  SYNOPSIS
    my_rnd()
    rand_st    INOUT  Structure used for number generation
    
  RETURN VALUE
    generated pseudo random number

  NOTE:
    This is codes so that it can be called by two threads at the same time
    with minimum impact.
    (As the number is supposed to be random, it doesn't matter much if
    rand->seed1 or rand->seed2 are updated with slightly wrong numbers or
    if two threads gets the same number.
*/

double my_rnd(struct my_rnd_struct *rand_st)
{
  unsigned long seed1;
  seed1= (rand_st->seed1*3+rand_st->seed2) % rand_st->max_value;
  rand_st->seed2=(seed1+rand_st->seed2+33) % rand_st->max_value;
  rand_st->seed1= seed1;
  return (((double) seed1)/rand_st->max_value_dbl);
}

