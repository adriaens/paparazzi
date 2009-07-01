/*
 * Copyright (C) 2008 Joby Energy
 *  
 *
 * paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with paparazzi; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA. 
 *
 */

/** \file csc_autpilot.h
 */

#ifndef __CSC_AUTOPILOT_H__
#define __CSC_AUTOPILOT_H__

#include "types.h"
#include "std.h"
#include "pprz_algebra_float.h"

struct control_gains {
  float pitch_kp;
  float pitch_kd;
  float pitch_ki;
  float roll_kp;
  float roll_kd;
  float roll_ki;
  float yaw_kp;
  float yaw_kd;
  float yaw_ki;
};

struct control_reference {
  struct FloatEulers eulers;
  struct FloatRates rates;
  struct FloatEulers eulers_i;
};

struct control_trims {
  int elevator;
  int aileron;
  int rudder;
};

extern struct control_gains csc_gains;
extern struct control_reference csc_reference;
extern struct control_trims csc_trims;
extern float csc_yaw_weight;

void csc_autopilot_set_roll_ki(float ki);
void csc_autopilot_set_pitch_ki(float ki);
void csc_autopilot_set_yaw_ki(float ki);

void csc_ap_init( void );
void csc_ap_periodic (void );
void csc_ap_set_trims (void );
void csc_ap_clear_ierrors (void );


#endif 