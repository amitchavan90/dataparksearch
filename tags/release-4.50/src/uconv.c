/* Copyright (C) 2003-2006 Datapark corp. All rights reserved.
   Copyright (C) 2000-2002 Lavtech.com corp. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
*/

#include "dps_config.h"
#include <stdio.h>
#include "dps_uniconv.h"


void __DPSCALL DpsConvInit(DPS_CONV *cnv, DPS_CHARSET *from, DPS_CHARSET *to, char *CharsToEscape, int fl) {
  cnv->from=from;
  cnv->to=to;
  cnv->flags=fl;
  cnv->ibytes=0;
  cnv->obytes=0;
  cnv->icodes = cnv->ocodes = 1; /* default for old functions */
  cnv->istate = cnv->ostate = 0; /* default state */
  cnv->CharsToEscape = CharsToEscape;
}

static int dps_ENTITYprint(char *dest, char sym, dpsunicode_t n) {
  dpsunicode_t mask = 10000000;
  register dpsunicode_t num = n, dig;
  int was_lead = 0;
  char *d = dest;
  *d = sym; d++;
  *d = '#'; d++;
  while(mask > 0) {
    dig = n / mask;
    if (dig > 0 || was_lead) {
      *d = (char)(dig + 0x30); d++;
      was_lead = 1;
    }
    n -= dig * mask;
    mask /= 10;
  }
  *d = ';'; d++;
  return (int)(d - dest);
}

int __DPSCALL DpsConv(DPS_CONV *c, char *d, size_t dlen,const char *s, size_t slen) {
  size_t	i, codes;
  int           res;
  dpsunicode_t  wc[32]; /* Are 32 is enough? */
  dpsunicode_t  zero = 0;
  char		*d_o=d;
  const char	*s_e=s+slen;
  char		*d_e=d+dlen;
  const char	*s_o=s;
  
  c->istate = 0; /* set default state */
  c->ostate = 0; /* set default state */
  while(s<s_e && d<d_e){
    
    res=c->from->mb_wc(c, c->from, wc,
                       (const unsigned char*)s,
                       (const unsigned char*)s_e);
    if (res > 0) {
      s+=res;
    } else if ((res==DPS_CHARSET_ILSEQ) || (res==DPS_CHARSET_ILSEQ2) || (res==DPS_CHARSET_ILSEQ3) || (res==DPS_CHARSET_ILSEQ4) 
	     || (res==DPS_CHARSET_ILSEQ5) || (res==DPS_CHARSET_ILSEQ6)) {

	switch (res) {
	case DPS_CHARSET_ILSEQ6: s++;
	case DPS_CHARSET_ILSEQ5: s++;
	case DPS_CHARSET_ILSEQ4: s++;
	case DPS_CHARSET_ILSEQ3: s++;
	case DPS_CHARSET_ILSEQ2: s++;
	case DPS_CHARSET_ILSEQ:
	default: s++; break;
	}
        wc[0] = '?';
    } else  break;

    codes = c->ocodes;
    for(i = 0; i < codes; i += c->icodes) {
outp:
      if (wc[i] == 0) goto outaway;
      res = c->to->wc_mb(c, c->to, &wc[i], (unsigned char*)d, (unsigned char*)d_e);
      if (res > 0) {
	d += res;
      } else if (res == DPS_CHARSET_ILUNI && wc[i] != '?') {
	if (c->flags & DPS_RECODE_HTML_TO) {
	  if (d_e-d > 11) {
/*	    res = sprintf(d, "&#%d;", (wc[i] & 0xFFFFFF));*/
	    res = dps_ENTITYprint(d, '&', (wc[i] & 0xFFFFFF));
	    d += res;
	  }
	  else
	    break;
	} else if (c->flags & DPS_RECODE_URL_TO) {
	  if (d_e-d > 11) {
/*	    res = sprintf(d, "!#%d;", (wc[i] & 0xFFFFFF));*/
	    res = dps_ENTITYprint(d, '!', (wc[i] & 0xFFFFFF));
	    d += res;
	  }
	  else
	    break;
	} else {
	  wc[i] = '?';
	  goto outp;
	}
      } else
	goto outaway;
    }
  }

outaway:  
  if(d <= d_e) {
    res = c->to->wc_mb(c, c->to, &zero, (unsigned char*)d, (unsigned char*)d_e);
  }
  c->ibytes=s-s_o;
  return (c->obytes = d - d_o );
}

__C_LINK const char * __DPSCALL DpsCsGroup(const DPS_CHARSET *cs) {
     switch(cs->family){
          case DPS_CHARSET_ARABIC       :    return "Arabic";
          case DPS_CHARSET_ARMENIAN     :    return "Armenian";
          case DPS_CHARSET_BALTIC       :    return "Baltic";
          case DPS_CHARSET_CELTIC       :    return "Celtic";
          case DPS_CHARSET_CENTRAL:          return "Central European";
          case DPS_CHARSET_CHINESE_SIMPLIFIED:    return "Chinese Simplified";
          case DPS_CHARSET_CHINESE_TRADITIONAL:   return "Chinese Traditional";
          case DPS_CHARSET_CYRILLIC     :    return "Cyrillic";
          case DPS_CHARSET_GREEK        :    return "Greek";
          case DPS_CHARSET_HEBREW       :    return "Hebrew";
          case DPS_CHARSET_ICELANDIC    :    return "Icelandic";
          case DPS_CHARSET_JAPANESE     :    return "Japanese";
          case DPS_CHARSET_KOREAN       :    return "Korean";
          case DPS_CHARSET_NORDIC       :    return "Nordic";
          case DPS_CHARSET_SOUTHERN     :    return "South Eur";
          case DPS_CHARSET_THAI         :    return "Thai";
          case DPS_CHARSET_TURKISH      :    return "Turkish";
          case DPS_CHARSET_UNICODE      :    return "Unicode";
          case DPS_CHARSET_VIETNAMESE   :    return "Vietnamese";
          case DPS_CHARSET_WESTERN      :    return "Western";
          case DPS_CHARSET_GEORGIAN     :    return "Georgian";
          case DPS_CHARSET_INDIAN       :    return "Indian";
          case DPS_CHARSET_LAO          :    return "Lao";
          case DPS_CHARSET_IRANIAN      :    return "Iranian";
          case DPS_CHARSET_TAJIK        :    return "Tajik";
          default                       :    return "Unknown";
     }
}

