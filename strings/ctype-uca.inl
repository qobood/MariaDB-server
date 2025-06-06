/*
  Copyright (c) 2018, 2020, MariaDB Corporation

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "ctype-mb.h"

#ifndef MY_FUNCTION_NAME
#error MY_FUNCTION_NAME is not defined
#endif
#ifndef MY_MB_WC
#error MY_MB_WC is not defined
#endif
#ifndef MY_LIKE_RANGE
#error MY_LIKE_RANGE is not defined
#endif
#ifndef MY_UCA_ASCII_OPTIMIZE
#error MY_ASCII_OPTIMIZE is not defined
#endif
#ifndef MY_UCA_COMPILE_CONTRACTIONS
#error MY_UCA_COMPILE_CONTRACTIONS is not defined
#endif
#ifndef MY_UCA_COLL_INIT
#error MY_UCA_COLL_INIT is not defined
#endif


#include "ctype-uca-scanner_next.inl"
#define SCANNER_NEXT_NCHARS
#include "ctype-uca-scanner_next.inl"

/*
  Compares two strings according to the collation

  SYNOPSIS:
    strnncoll_onelevel()
    cs		Character set information
    level       Weight level (0 primary, 1 secondary, 2 tertiary, etc)
    s		First string
    slen	First string length
    t		Second string
    tlen	Seconf string length
    level	DUCETweight level
  
  NOTES:
    Initializes two weight scanners and gets weights
    corresponding to two strings in a loop. If weights are not
    the same at some step then returns their difference.
    
    In the while() comparison these situations are possible:
    1. (s_res>0) and (t_res>0) and (s_res == t_res)
       Weights are the same so far, continue comparison
    2. (s_res>0) and (t_res>0) and (s_res!=t_res)
       A difference has been found, return.
    3. (s_res>0) and (t_res<0)
       We have reached the end of the second string, or found
       an illegal multibyte sequence in the second string.
       Return a positive number, i.e. the first string is bigger.
    4. (s_res<0) and (t_res>0)   
       We have reached the end of the first string, or found
       an illegal multibyte sequence in the first string.
       Return a negative number, i.e. the second string is bigger.
    5. (s_res<0) and (t_res<0)
       Both scanners returned -1. It means we have riched
       the end-of-string of illegal-sequence in both strings
       at the same time. Return 0, strings are equal.
    
  RETURN
    Difference between two strings, according to the collation:
    0               - means strings are equal
    negative number - means the first string is smaller
    positive number - means the first string is bigger
*/

static int
MY_FUNCTION_NAME(strnncoll_onelevel)(CHARSET_INFO *cs, 
                                     const MY_UCA_WEIGHT_LEVEL *level,
                                     const uchar *s, size_t slen,
                                     const uchar *t, size_t tlen,
                                     my_bool t_is_prefix)
{
  my_uca_scanner sscanner;
  my_uca_scanner tscanner;
  my_uca_scanner_param param;
  int s_res;
  int t_res;

#if MY_UCA_ASCII_OPTIMIZE
{
  size_t prefix= my_uca_level_booster_equal_prefix_length(level->booster,
                                                          s, slen, t, tlen);
  s+= prefix, slen-= prefix;
  t+= prefix, tlen-= prefix;
}
#endif

  my_uca_scanner_param_init(&param, cs, level);
  my_uca_scanner_init_any(&sscanner, s, slen);
  my_uca_scanner_init_any(&tscanner, t, tlen);
  
  do
  {
    s_res= MY_FUNCTION_NAME(scanner_next)(&sscanner, &param);
    t_res= MY_FUNCTION_NAME(scanner_next)(&tscanner, &param);
  } while ( s_res == t_res && s_res >0);
  
  return  (t_is_prefix && t_res < 0) ? 0 : (s_res - t_res);
}


/*
  One-level, PAD SPACE.
*/
static int
MY_FUNCTION_NAME(strnncoll)(CHARSET_INFO *cs,
                            const uchar *s, size_t slen,
                            const uchar *t, size_t tlen,
                            my_bool t_is_prefix)
{
  return MY_FUNCTION_NAME(strnncoll_onelevel)(cs, &cs->uca->level[0],
                                              s, slen, t, tlen, t_is_prefix);
}


/*
  Multi-level, PAD SPACE.
*/
static int
MY_FUNCTION_NAME(strnncoll_multilevel)(CHARSET_INFO *cs,
                                       const uchar *s, size_t slen,
                                       const uchar *t, size_t tlen,
                                       my_bool t_is_prefix)
{
  uint i, level_flags= cs->levels_for_order;
  for (i= 0; level_flags; i++, level_flags>>= 1)
  {
    int ret;
    if (!(level_flags & 1))
      continue;
    ret= MY_FUNCTION_NAME(strnncoll_onelevel)(cs, &cs->uca->level[i],
                                              s, slen, t, tlen,
                                              t_is_prefix);
    if (ret)
       return ret;
  }
  return 0;
}


/*
  Compares two strings according to the collation,
  ignoring trailing spaces.

  SYNOPSIS:
    strnncollsp_onelevel()
    cs		Character set information
    level       UCA weight level
    s		First string
    slen	First string length
    t		Second string
    tlen	Seconf string length
    level	DUCETweight level

  NOTES:
    Works exactly the same with my_strnncoll_uca(),
    but ignores trailing spaces.

    In the while() comparison these situations are possible:
    1. (s_res>0) and (t_res>0) and (s_res == t_res)
       Weights are the same so far, continue comparison
    2. (s_res>0) and (t_res>0) and (s_res!=t_res)
       A difference has been found, return.
    3. (s_res>0) and (t_res<0)
       We have reached the end of the second string, or found
       an illegal multibyte sequence in the second string.
       Compare the first string to an infinite array of
       space characters until difference is found, or until
       the end of the first string.
    4. (s_res<0) and (t_res>0)
       We have reached the end of the first string, or found
       an illegal multibyte sequence in the first string.
       Compare the second string to an infinite array of
       space characters until difference is found or until
       the end of the second steing.
    5. (s_res<0) and (t_res<0)
       Both scanners returned -1. It means we have riched
       the end-of-string of illegal-sequence in both strings
       at the same time. Return 0, strings are equal.

  RETURN
    Difference between two strings, according to the collation:
    0               - means strings are equal
    negative number - means the first string is smaller
    positive number - means the first string is bigger
*/

static int
MY_FUNCTION_NAME(strnncollsp_onelevel)(CHARSET_INFO *cs,
                                       const MY_UCA_WEIGHT_LEVEL *level,
                                       const uchar *s, size_t slen,
                                       const uchar *t, size_t tlen)
{
  my_uca_scanner sscanner, tscanner;
  my_uca_scanner_param param;
  int s_res, t_res;

#if MY_UCA_ASCII_OPTIMIZE
{
  size_t prefix= my_uca_level_booster_equal_prefix_length(level->booster,
                                                          s, slen, t, tlen);
  s+= prefix, slen-= prefix;
  t+= prefix, tlen-= prefix;
}
#endif

  my_uca_scanner_param_init(&param, cs, level);
  my_uca_scanner_init_any(&sscanner, s, slen);
  my_uca_scanner_init_any(&tscanner, t, tlen);

  do
  {
    s_res= MY_FUNCTION_NAME(scanner_next)(&sscanner, &param);
    t_res= MY_FUNCTION_NAME(scanner_next)(&tscanner, &param);
  } while ( s_res == t_res && s_res >0);

  if (s_res > 0 && t_res < 0)
  {
    /* Calculate weight for SPACE character */
    t_res= my_space_weight(level);

    /* compare the first string to spaces */
    do
    {
      if (s_res != t_res)
        return (s_res - t_res);
      s_res= MY_FUNCTION_NAME(scanner_next)(&sscanner, &param);
    } while (s_res > 0);
    return 0;
  }

  if (s_res < 0 && t_res > 0)
  {
    /* Calculate weight for SPACE character */
    s_res= my_space_weight(level);

    /* compare the second string to spaces */
    do
    {
      if (s_res != t_res)
        return (s_res - t_res);
      t_res= MY_FUNCTION_NAME(scanner_next)(&tscanner, &param);
    } while (t_res > 0);
    return 0;
  }

  return ( s_res - t_res );
}


/*
  One-level, PAD SPACE
*/
static int
MY_FUNCTION_NAME(strnncollsp)(CHARSET_INFO *cs,
                              const uchar *s, size_t slen,
                              const uchar *t, size_t tlen)
{
  return MY_FUNCTION_NAME(strnncollsp_onelevel)(cs, &cs->uca->level[0],
                                                s, slen, t, tlen);
}


/*
  One-level, NO PAD
*/
static int
MY_FUNCTION_NAME(strnncollsp_nopad)(CHARSET_INFO *cs,
                                    const uchar *s, size_t slen,
                                    const uchar *t, size_t tlen)
{
  return MY_FUNCTION_NAME(strnncoll_onelevel)(cs, &cs->uca->level[0],
                                              s, slen, t, tlen, FALSE);
}


/*
  Multi-level, PAD SPACE
*/
static int
MY_FUNCTION_NAME(strnncollsp_multilevel)(CHARSET_INFO *cs,
                                         const uchar *s, size_t slen,
                                         const uchar *t, size_t tlen)
{
  uint i, level_flags= cs->levels_for_order;
  for (i= 0; level_flags; i++, level_flags>>= 1)
  {
    int ret;
    if (!(level_flags & 1))
      continue;
    ret= MY_FUNCTION_NAME(strnncollsp_onelevel)(cs, &cs->uca->level[i],
                                                s, slen, t, tlen);
    if (ret)
      return ret;
  }
  return 0;
}


/*
  Multi-level, NO PAD
*/
static int
MY_FUNCTION_NAME(strnncollsp_nopad_multilevel)(CHARSET_INFO *cs,
                                               const uchar *s, size_t slen,
                                               const uchar *t, size_t tlen)
{
  uint i, level_flags;
  int ret;

  /* Compare only the primary level using NO PAD */
  if ((ret= MY_FUNCTION_NAME(strnncoll_onelevel)(cs, &cs->uca->level[0],
                                                 s, slen, t, tlen, FALSE)))
    return ret;

  /*
    Compare the other levels using PAD SPACE.
    These are Unicode-14.0.0 DUCTET weights:

0020  ; [*0209.0020.0002] # SPACE

0035  ; [.2070.0020.0002] # DIGIT FIVE
248C  ; [.2070.0020.0004][*0281.0020.0004] # DIGIT FIVE FULL STOP

0041  ; [.2075.0020.0008] # LATIN CAPITAL LETTER A
0061  ; [.2075.0020.0002] # LATIN SMALL LETTER A
00C1  ; [.2075.0020.0008][.0000.0024.0002] # LATIN CAPITAL LETTER A WITH ACUTE
00E1  ; [.2075.0020.0002][.0000.0024.0002] # LATIN SMALL LETTER A WITH ACUTE

    Examples demonstrating that it's important to use PAD SPACE
    on the tertiary level:

    The third level weights for "SMALL LETTER A"
    - U+0061 produces one weight 0002
    - U+00E1 produces two weights 0002+0002
      For _ai_cs collations these two letters must be equal.
      Therefore, the difference in trailing 0002 should be ignored.

    The third level weights for "CAPITAL LETTER A"
    - U+0041 produces one weight 0008
    - U+00C1 produces two weights 0008+0002
      For _ai_cs collations these two letters must be equal.
      Therefore, the difference in trailing 0002 should be ignored.

    Examples demonstrating that it's important to use PAD SPACE
    on the secondary level:

    When we implement variable shifted alternative weighting collations,
    U+0035 will be equal to U+248C on the primary level in these collations.
    The second level weights for "DIGIT FIVE" are:
    - U+0035 produces one weight 0020
    - U+248C produces two weights 0020+0020.
    The difference for these two characters must be found only
    on the tertiary level. Therefore, the trailing 0020 should be ignored.
  */

  for (i= 1, level_flags= cs->levels_for_order >> 1;
       level_flags;
       i++, level_flags>>= 1)
  {
    if (!(level_flags & 1))
      continue;
    ret= MY_FUNCTION_NAME(strnncollsp_onelevel)(cs, &cs->uca->level[i],
                                                s, slen, t, tlen);
    if (ret)
       return ret;
  }
  return 0;
}


/*
  Scan the next weight and perform space padding
  or trimming according to "nchars".
*/
static inline weight_and_nchars_t
MY_FUNCTION_NAME(scanner_next_pad_trim)(my_uca_scanner *scanner,
                                        my_uca_scanner_param *param,
                                        size_t nchars,
                                        uint flags,
                                        uint *generated)
{
  weight_and_nchars_t res;
  if (nchars > 0 ||
      scanner->wbeg[0] /* Some weights from a previous expansion left */)
  {
    if ((res= MY_FUNCTION_NAME(scanner_next_with_nchars)(scanner, param,
                                                         nchars)).weight < 0)
    {
      /*
        We reached the end of the string, but the caller wants more weights.
        Perform space padding.
      */
      res.weight=
        flags & MY_STRNNCOLLSP_NCHARS_EMULATE_TRIMMED_TRAILING_SPACES ?
        my_space_weight(param->level) : 0;

      (*generated)++;
      res.nchars++; /* Count all ignorable characters and the padded space */
      if (res.nchars > nchars)
      {
        /*
          We scanned a number of ignorable characters at the end of the
          string and reached the "nchars" limit, so the virtual padded space
          does not fit. This is possible with CONCAT('a', x'00') with
          nchars=2 on the second iteration when we scan the x'00'.
        */
        if (param->cs->state & MY_CS_NOPAD)
          res.weight= 0;
        res.nchars= (uint) nchars;
      }
    }
    else if (res.nchars > nchars)
    {
      /*
        We scanned the next collation element, but it does not fit into
        the "nchars" limit. This is possible in case of:
        - A contraction, e.g. Czech 'ch' with nchars=1
        - A sequence of ignorable characters followed by non-ignorable ones,
          e.g. CONCAT(x'00','a') with nchars=1.
        Perform trimming.
      */
      res.weight= param->cs->state & MY_CS_NOPAD ?
                  0 : my_space_weight(param->level);
      res.nchars= (uint) nchars;
      (*generated)++;
    }
  }
  else
  {
    /* The caller wants nchars==0. Perform trimming. */
    res.weight= param->cs->state & MY_CS_NOPAD ?
                0 : my_space_weight(param->level);
    res.nchars= 0;
    (*generated)++;
  }
  return res;
}


static int
MY_FUNCTION_NAME(strnncollsp_nchars_onelevel)(CHARSET_INFO *cs,
                                              const MY_UCA_WEIGHT_LEVEL *level,
                                              const uchar *s, size_t slen,
                                              const uchar *t, size_t tlen,
                                              size_t nchars,
                                              uint flags)
{
  my_uca_scanner sscanner;
  my_uca_scanner tscanner;
  my_uca_scanner_param param;
  size_t s_nchars_left= nchars;
  size_t t_nchars_left= nchars;

/*
TODO: strnncollsp_nchars_onelevel
#if MY_UCA_ASCII_OPTIMIZE
{
  size_t prefix= my_uca_level_booster_equal_prefix_length(level->booster,
                                                          s, slen, t, tlen);
  s+= prefix, slen-= prefix;
  t+= prefix, tlen-= prefix;
}
#endif
*/

  my_uca_scanner_param_init(&param, cs, level);
  my_uca_scanner_init_any(&sscanner, s, slen);
  my_uca_scanner_init_any(&tscanner, t, tlen);

  for ( ; ; )
  {
    weight_and_nchars_t s_res;
    weight_and_nchars_t t_res;
    uint generated= 0;
    int diff;

    s_res= MY_FUNCTION_NAME(scanner_next_pad_trim)(&sscanner, &param,
                                                   s_nchars_left,
                                                   flags, &generated);
    t_res= MY_FUNCTION_NAME(scanner_next_pad_trim)(&tscanner, &param,
                                                   t_nchars_left,
                                                   flags, &generated);
    if ((diff= (s_res.weight - t_res.weight)))
      return diff;

    if (generated == 2)
    {
      if ((cs->state & MY_CS_NOPAD) &&
          (flags & MY_STRNNCOLLSP_NCHARS_EMULATE_TRIMMED_TRAILING_SPACES))
      {
        /*
          Both values are auto-generated. There's no real data any more.
          We need to handle the remaining virtual trailing spaces.
          The two strings still have s_nchars_left and t_nchars_left imaginary
          trailing spaces at the end. If s_nchars_left != t_nchars_left,
          the strings will be not equal in case of a NOPAD collation.

          Example:
          "B" is German "U+00DF LATIN SMALL LETTER SHARP S"
          When we have these values in a
          CHAR(3) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_nopad_ci
          column:
          'B  '        (one character, two trailing spaces)
          'ss '        (two characters, one trailing space)
          The 'B  ' is greater than the 'ss '.
          They are compared in the following steps:
            1. 'B' == 'ss'
            2. ' ' == ' '
            3. ' ' >   ''

          We need to emulate the same behavior in this function even if
          it's called with strings 'B' and 'ss' (with space trimmed).
          The side which has more remaining virtual spaces at the end
          is greater.
        */
        if (s_nchars_left < t_nchars_left)
          return -1;
        if (s_nchars_left > t_nchars_left)
          return +1;
      }
      return 0;
    }

    DBUG_ASSERT(s_nchars_left >= s_res.nchars);
    DBUG_ASSERT(t_nchars_left >= t_res.nchars);
    s_nchars_left-= s_res.nchars;
    t_nchars_left-= t_res.nchars;
  }

  return 0;
}


/*
  One-level collations.
*/
static int
MY_FUNCTION_NAME(strnncollsp_nchars)(CHARSET_INFO *cs,
                                     const uchar *s, size_t slen,
                                     const uchar *t, size_t tlen,
                                     size_t nchars,
                                     uint flags)
{
  return MY_FUNCTION_NAME(strnncollsp_nchars_onelevel)(cs, &cs->uca->level[0],
                                                       s, slen, t, tlen,
                                                       nchars, flags);
}


/*
  Multi-level collations.
*/
static int
MY_FUNCTION_NAME(strnncollsp_nchars_multilevel)(CHARSET_INFO *cs,
                                                const uchar *s, size_t slen,
                                                const uchar *t, size_t tlen,
                                                size_t nchars,
                                                uint flags)
{
  uint i, level_flags= cs->levels_for_order;
  for (i= 0; level_flags; i++, level_flags>>= 1)
  {
    int ret;
    if (!(level_flags & 1))
      continue;
    ret= MY_FUNCTION_NAME(strnncollsp_nchars_onelevel)(cs,
                                                       &cs->uca->level[i],
                                                       s, slen,
                                                       t, tlen,
                                                       nchars, flags);
    if (ret)
       return ret;
  }
  return 0;
}


/*
  Calculates hash value for the given string,
  according to the collation, and ignoring trailing spaces.

  SYNOPSIS:
    hash_sort()
    cs		Character set information
    s		String
    slen	String's length
    n1		First hash parameter
    n2		Second hash parameter

  NOTES:
    Scans consequently weights and updates
    hash parameters n1 and n2. In a case insensitive collation,
    upper and lower case of the same letter will return the same
    weight sequence, and thus will produce the same hash values
    in n1 and n2.

    This functions is used for one-level and for multi-level collations.
    We intentionally use only primary level in multi-level collations.
    This helps to have PARTITION BY KEY put primarily equal records
    into the same partition. E.g. in utf8mb3_thai_520_ci records that differ
    only in tone marks go into the same partition.

  RETURN
    N/A
*/

static void
MY_FUNCTION_NAME(hash_sort)(CHARSET_INFO *cs,
                            const uchar *s, size_t slen,
                            ulong *nr1, ulong *nr2)
{
  int   s_res;
  my_uca_scanner scanner;
  my_uca_scanner_param param;
  int space_weight= my_space_weight(&cs->uca->level[0]);
  register ulong m1= *nr1, m2= *nr2;
  DBUG_ASSERT(s); /* Avoid UBSAN nullptr-with-offset */

  my_uca_scanner_param_init(&param, cs, &cs->uca->level[0]);
  my_uca_scanner_init_any(&scanner, s, slen);

  while ((s_res= MY_FUNCTION_NAME(scanner_next)(&scanner, &param)) >0)
  {
    if (s_res == space_weight)
    {
      /* Combine all spaces to be able to skip end spaces */
      uint count= 0;
      do
      {
        count++;
        if ((s_res= MY_FUNCTION_NAME(scanner_next)(&scanner, &param)) <= 0)
        {
          /* Skip strings at end of string */
          goto end;
        }
      }
      while (s_res == space_weight);

      /* Add back that has for the space characters */
      do
      {
        /*
          We can't use MY_HASH_ADD_16() here as we, because of a misstake
          in the original code, where we added the 16 byte variable the
          opposite way.  Changing this would cause old partitioned tables
          to fail.
        */
        MY_HASH_ADD(m1, m2, space_weight >> 8);
        MY_HASH_ADD(m1, m2, space_weight & 0xFF);
      }
      while (--count != 0);

    }
    /* See comment above why we can't use MY_HASH_ADD_16() */
    MY_HASH_ADD(m1, m2, s_res >> 8);
    MY_HASH_ADD(m1, m2, s_res & 0xFF);
  }
end:
  *nr1= m1;
  *nr2= m2;
}


static void
MY_FUNCTION_NAME(hash_sort_nopad)(CHARSET_INFO *cs,
                                  const uchar *s, size_t slen,
                                  ulong *nr1, ulong *nr2)
{
  int   s_res;
  my_uca_scanner scanner;
  my_uca_scanner_param param;
  register ulong m1= *nr1, m2= *nr2;
  DBUG_ASSERT(s); /* Avoid UBSAN nullptr-with-offset */

  my_uca_scanner_param_init(&param, cs, &cs->uca->level[0]);
  my_uca_scanner_init_any(&scanner, s, slen);

  while ((s_res= MY_FUNCTION_NAME(scanner_next)(&scanner, &param)) >0)
  {
    /* See comment above why we can't use MY_HASH_ADD_16() */
    MY_HASH_ADD(m1, m2, s_res >> 8);
    MY_HASH_ADD(m1, m2, s_res & 0xFF);
  }
  *nr1= m1;
  *nr2= m2;
}



/*
  For the given string creates its "binary image", suitable
  to be used in binary comparison, i.e. in memcmp(). 
  
  SYNOPSIS:
    my_strnxfrm_uca()
    cs		Character set information
    dst		Where to write the image
    dstlen	Space available for the image, in bytes
    src		The source string
    srclen	Length of the source string, in bytes
  
  NOTES:
    In a loop, scans weights from the source string and writes
    them into the binary image. In a case insensitive collation,
    upper and lower cases of the same letter will produce the
    same image subsequences. When we have reached the end-of-string
    or found an illegal multibyte sequence, the loop stops.

    It is impossible to restore the original string using its
    binary image. 
    
    Binary images are used for bulk comparison purposes,
    e.g. in ORDER BY, when it is more efficient to create
    a binary image and use it instead of weight scanner
    for the original strings for every comparison.
  
  RETURN
    Number of bytes that have been written into the binary image.
*/

static my_strnxfrm_ret_t
MY_FUNCTION_NAME(strnxfrm_onelevel_internal)(CHARSET_INFO *cs,
                                             MY_UCA_WEIGHT_LEVEL *level,
                                             uchar *dst, uchar *de,
                                             uint *nweights,
                                             const uchar *src, size_t srclen)
{
  my_uca_scanner scanner;
  my_uca_scanner_param param;
  int s_res;
  const uchar *src0= src;
  const uchar *dst0= dst;
  const uchar *de2= de - 1; /* Last position where 2 bytes fit */

  DBUG_ASSERT(src || !srclen);

#if MY_UCA_ASCII_OPTIMIZE && !MY_UCA_COMPILE_CONTRACTIONS
 /*
    Fast path for the ASCII range with no contractions.
  */
  {
    const uint16 *weights0= level->weights[0];
    uint lengths0= level->lengths[0];
    for ( ; ; src++, srclen--)
    {
      const uint16 *weight;
      if (!srclen)
        return my_strnxfrm_ret_construct(dst - dst0, src - src0, 0);
      if (*src > 0x7F)
        break;              /* Non-ASCII */

      weight= weights0 + (((uint) *src) * lengths0);
      if (!(s_res= *weight))
        continue;           /* Ignorable */
      if (weight[1])        /* Expansion (e.g. in a user defined collation */
        break;

      if (!*nweights)
        return my_strnxfrm_ret_construct(dst - dst0, src - src0,
                                      MY_STRNXFRM_TRUNCATED_WEIGHT_REAL_CHAR);
      /* Here we have a character with extactly one 2-byte UCA weight */
      if (dst < de2)        /* Most typical case is when both bytes fit */
      {
        *dst++= s_res >> 8;
        *dst++= s_res & 0xFF;
        (*nweights)--;
        continue;
      }
      if (dst >= de)        /* No space left in "dst" */
        return my_strnxfrm_ret_construct(dst - dst0, src - src0,
                                       MY_STRNXFRM_TRUNCATED_WEIGHT_REAL_CHAR);
      *dst++= s_res >> 8;   /* There is space only for one byte */
      (*nweights)--;
      return my_strnxfrm_ret_construct(dst - dst0, src + 1 - src0,
                                       MY_STRNXFRM_TRUNCATED_WEIGHT_REAL_CHAR);
    }
  }
#endif

  my_uca_scanner_param_init(&param, cs, level);
  my_uca_scanner_init_any(&scanner, src, srclen);

  for (; (s_res= MY_FUNCTION_NAME(scanner_next)(&scanner, &param)) > 0 ;
       (*nweights)--)
  {
    if (!*nweights)
      return my_strnxfrm_ret_construct(dst - dst0, scanner.sbeg - src0,
                                 MY_STRNXFRM_TRUNCATED_WEIGHT_REAL_CHAR);
    if (dst < de2)
    {
      *dst++= s_res >> 8;
      *dst++= s_res & 0xFF;
    }
    else
    {
      if (dst < de)
        *dst++= s_res >> 8;
      return my_strnxfrm_ret_construct(dst - dst0, scanner.sbeg - src0,
                                 MY_STRNXFRM_TRUNCATED_WEIGHT_REAL_CHAR);
    }
  }
  return my_strnxfrm_ret_construct(dst - dst0, scanner.sbeg - src0,
                         my_uca_scanner_next_expansion_weight(&scanner) > 0 ?
                         MY_STRNXFRM_TRUNCATED_WEIGHT_REAL_CHAR : 0);
}


static my_strnxfrm_ret_t
MY_FUNCTION_NAME(strnxfrm_onelevel)(CHARSET_INFO *cs,
                                    MY_UCA_WEIGHT_LEVEL *level,
                                    uchar *dst, uchar *de, uint nweights,
                                    const uchar *src, size_t srclen, uint flags)
{
  my_strnxfrm_ret_t rc= MY_FUNCTION_NAME(strnxfrm_onelevel_internal)(cs, level,
                                                            dst, de, &nweights,
                                                            src, srclen);
  DBUG_ASSERT(dst + rc.m_result_length <= de);
  if (nweights && (flags & MY_STRXFRM_PAD_WITH_SPACE))
  {
    my_strnxfrm_pad_ret_t rcpad= my_strnxfrm_uca_padn(dst + rc.m_result_length,
                                                      de, nweights,
                                                      my_space_weight(level));
    my_strnxfrm_ret_join_pad(&rc, &rcpad);
    DBUG_ASSERT(dst + rc.m_result_length <= de);
  }
  my_strxfrm_desc_and_reverse(dst, dst + rc.m_result_length, flags, 0);
  return rc;
}


static my_strnxfrm_ret_t
MY_FUNCTION_NAME(strnxfrm_nopad_onelevel)(CHARSET_INFO *cs,
                                          MY_UCA_WEIGHT_LEVEL *level,
                                          uchar *dst, uchar *de, uint nweights,
                                          const uchar *src, size_t srclen,
                                          uint flags)
{
  my_strnxfrm_ret_t rc= MY_FUNCTION_NAME(strnxfrm_onelevel_internal)(cs, level,
                                                           dst, de, &nweights,
                                                           src, srclen);
  DBUG_ASSERT(dst + rc.m_result_length <= de);
  /*  Pad with the minimum possible weight on this level */
  if (nweights && (flags & MY_STRXFRM_PAD_WITH_SPACE))
  {
    my_strnxfrm_pad_ret_t rcpad= my_strnxfrm_uca_padn(dst + rc.m_result_length,
                                                  de, nweights,
                                                  min_weight_on_level(level));
    my_strnxfrm_ret_join_pad(&rc, &rcpad);
    DBUG_ASSERT(dst + rc.m_result_length <= de);
  }
  my_strxfrm_desc_and_reverse(dst, dst + rc.m_result_length, flags, 0);
  return rc;
}


static my_strnxfrm_ret_t
MY_FUNCTION_NAME(strnxfrm)(CHARSET_INFO *cs,
                           uchar *dst, size_t dstlen, uint nweights,
                           const uchar *src, size_t srclen, uint flags)
{
  uchar *d0= dst;
  uchar *de= dst + dstlen;
  my_strnxfrm_ret_t rc;

  /*
    There are two ways to handle trailing spaces for PAD SPACE collations:
    1. Keep trailing spaces as they are, so have strnxfrm_onelevel() scan
       spaces as normal characters. This will call scanner_next() for every
       trailing space and calculate its weight using UCA weights.
    2. Strip trailing spaces before calling strnxfrm_onelevel(), as it will
       append weights for implicit spaces anyway, up to the desired key size.
       This will effectively generate exactly the same sortable key result.
    The latter is much faster.
  */

  if (flags & MY_STRXFRM_PAD_WITH_SPACE)
    srclen= my_ci_lengthsp(cs, (const char*) src, srclen);
  rc= MY_FUNCTION_NAME(strnxfrm_onelevel)(cs, &cs->uca->level[0],
                                          dst, de, nweights,
                                          src, srclen, flags);
  dst+= rc.m_result_length;
  /*
    This can probably be changed to memset(dst, 0, de - dst),
    like my_strnxfrm_uca_multilevel() does.
  */
  if ((flags & MY_STRXFRM_PAD_TO_MAXLEN) && dst < de)
    dst= my_strnxfrm_uca_pad(dst, de, my_space_weight(&cs->uca->level[0]));
  rc.m_result_length= dst - d0;
  return rc;
}


static my_strnxfrm_ret_t
MY_FUNCTION_NAME(strnxfrm_nopad)(CHARSET_INFO *cs,
                                 uchar *dst, size_t dstlen,
                                 uint nweights,
                                 const uchar *src, size_t srclen,
                                 uint flags)
{
  uchar *d0= dst;
  uchar *de= dst + dstlen;
  my_strnxfrm_ret_t rc= MY_FUNCTION_NAME(strnxfrm_nopad_onelevel)(cs,
                                                          &cs->uca->level[0],
                                                          dst, de, nweights,
                                                          src, srclen, flags);
  dst+= rc.m_result_length;
  if ((flags & MY_STRXFRM_PAD_TO_MAXLEN) && dst < de)
  {
    memset(dst, 0, de - dst);
    dst= de;
  }
  rc.m_result_length= dst - d0;
  return rc;
}


static my_strnxfrm_ret_t
MY_FUNCTION_NAME(strnxfrm_multilevel)(CHARSET_INFO *cs, 
                                      uchar *dst, size_t dstlen,
                                      uint nweights,
                                      const uchar *src, size_t srclen,
                                      uint flags)
{
  uint level_flags= cs->levels_for_order;
  uchar *d0= dst;
  uchar *de= dst + dstlen;
  uchar *de_for_levels= dst + dstlen;
  uint current_level;
  my_strnxfrm_ret_t rc= my_strnxfrm_ret_construct(0, 0, 0);

  for (current_level= 0; level_flags; current_level++, level_flags>>= 1)
  {
    if (!(level_flags & 1))
      continue;
    if (!(flags & MY_STRXFRM_LEVEL_ALL) ||
        (flags & (MY_STRXFRM_LEVEL1 << current_level)))
    {
      const my_strnxfrm_ret_t rc1= cs->state & MY_CS_NOPAD ?
           MY_FUNCTION_NAME(strnxfrm_nopad_onelevel)(cs,
                                          &cs->uca->level[current_level],
                                          dst, de_for_levels, nweights,
                                          src, srclen, flags) :
           MY_FUNCTION_NAME(strnxfrm_onelevel)(cs,
                                    &cs->uca->level[current_level],
                                    dst, de_for_levels, nweights,
                                    src, srclen, flags);
      rc.m_source_length_used+= rc1.m_source_length_used;
      rc.m_warnings|= rc1.m_warnings;
      dst+= rc1.m_result_length;
      DBUG_ASSERT(dst <= de);
      if (rc1.m_warnings)
      {
        if (rc1.m_warnings & MY_STRNXFRM_TRUNCATED_WEIGHT_REAL_CHAR)
           break;
        /*
          A weight for a padding space did not fit on the current level.
          Characters may be ignorable on this level, but non-ignorable
          on the next level. Let's continue with the next levels
          only to find non-ignorable characters and set
          MY_STRNXFRM_TRUNCATED_WEIGHT_REAL_CHAR if found.
          But let's set ds_for_levels to dst to prevent putting
          any weights into the destination buffer on the the next.
        */
        de_for_levels= dst;
      }
    }
  }

  if (dst < de && (flags & MY_STRXFRM_PAD_TO_MAXLEN))
  {
    memset(dst, 0, de - dst);
    dst= de;
  }

  rc.m_result_length= dst - d0;
  return rc;
}


/*
  One-level, PAD SPACE
*/
MY_COLLATION_HANDLER MY_FUNCTION_NAME(collation_handler)=
{
  MY_UCA_COLL_INIT,
  MY_FUNCTION_NAME(strnncoll),
  MY_FUNCTION_NAME(strnncollsp),
  MY_FUNCTION_NAME(strnncollsp_nchars),
  MY_FUNCTION_NAME(strnxfrm),
  my_strnxfrmlen_any_uca,
  MY_LIKE_RANGE,
  my_wildcmp_uca,
  my_instr_mb,
  MY_FUNCTION_NAME(hash_sort),
  my_propagate_complex,
  my_min_str_mb_simple,
  my_max_str_mb_simple,
  my_ci_get_id_uca,
  my_ci_get_collation_name_uca
};


/*
  One-level, NO PAD
  For character sets with mbminlen==1 use MY_LIKE_RANGE=my_like_range_mb
  For character sets with mbminlen>=2 use MY_LIKE_RANGE=my_like_range_generic
*/
MY_COLLATION_HANDLER MY_FUNCTION_NAME(collation_handler_nopad)=
{
  MY_UCA_COLL_INIT,
  MY_FUNCTION_NAME(strnncoll),
  MY_FUNCTION_NAME(strnncollsp_nopad),
  MY_FUNCTION_NAME(strnncollsp_nchars),
  MY_FUNCTION_NAME(strnxfrm_nopad),
  my_strnxfrmlen_any_uca,
  MY_LIKE_RANGE,    /* my_like_range_mb or my_like_range_generic */
  my_wildcmp_uca,
  my_instr_mb,
  MY_FUNCTION_NAME(hash_sort_nopad),
  my_propagate_complex,
  my_min_str_mb_simple_nopad,
  my_max_str_mb_simple,
  my_ci_get_id_uca,
  my_ci_get_collation_name_uca
};


/*
  Multi-level, PAD SPACE
*/
MY_COLLATION_HANDLER MY_FUNCTION_NAME(collation_handler_multilevel)=
{
  MY_UCA_COLL_INIT,
  MY_FUNCTION_NAME(strnncoll_multilevel),
  MY_FUNCTION_NAME(strnncollsp_multilevel),
  MY_FUNCTION_NAME(strnncollsp_nchars_multilevel),
  MY_FUNCTION_NAME(strnxfrm_multilevel),
  my_strnxfrmlen_any_uca_multilevel,
  MY_LIKE_RANGE,
  my_wildcmp_uca,
  my_instr_mb,
  MY_FUNCTION_NAME(hash_sort),
  my_propagate_complex,
  my_min_str_mb_simple,
  my_max_str_mb_simple,
  my_ci_get_id_uca,
  my_ci_get_collation_name_uca
};


/*
  Multi-level, NO PAD
*/
MY_COLLATION_HANDLER MY_FUNCTION_NAME(collation_handler_nopad_multilevel)=
{
  MY_UCA_COLL_INIT,
  MY_FUNCTION_NAME(strnncoll_multilevel),
  MY_FUNCTION_NAME(strnncollsp_nopad_multilevel),
  MY_FUNCTION_NAME(strnncollsp_nchars_multilevel),
  MY_FUNCTION_NAME(strnxfrm_multilevel),
  my_strnxfrmlen_any_uca_multilevel,
  MY_LIKE_RANGE,
  my_wildcmp_uca,
  my_instr_mb,
  MY_FUNCTION_NAME(hash_sort),
  my_propagate_complex,
  my_min_str_mb_simple_nopad,
  my_max_str_mb_simple,
  my_ci_get_id_uca,
  my_ci_get_collation_name_uca
};


MY_COLLATION_HANDLER_PACKAGE MY_FUNCTION_NAME(package)=
{
  &MY_FUNCTION_NAME(collation_handler),
  &MY_FUNCTION_NAME(collation_handler_nopad),
  &MY_FUNCTION_NAME(collation_handler_multilevel),
  &MY_FUNCTION_NAME(collation_handler_nopad_multilevel)
};


#undef MY_FUNCTION_NAME
#undef MY_MB_WC
#undef MY_LIKE_RANGE
#undef MY_UCA_ASCII_OPTIMIZE
#undef MY_UCA_COMPILE_CONTRACTIONS
#undef MY_UCA_COLL_INIT
