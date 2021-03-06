/**
 * MK4duo Firmware for 3D Printer, Laser and CNC
 *
 * Based on Marlin, Sprinter and grbl
 * Copyright (C) 2011 Camiel Gubbels / Erik van der Zalm
 * Copyright (C) 2013 Alberto Cotronei @MagoKimbra
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 */

/**
 * gcode.h
 *
 * Copyright (C) 2017 Alberto Cotronei @MagoKimbra
 */

#if ENABLED(DELTA_AUTO_CALIBRATION_2)

  #define CODE_G33

  void Calibration_cleanup(
    #if HOTENDS > 1
      const uint8_t old_tool_index
    #endif
  ) {
    #if ENABLED(DELTA_HOME_TO_SAFE_ZONE)
      mechanics.do_blocking_move_to_z(mechanics.delta_clip_start_height);
    #endif
    STOW_PROBE();
    printer.clean_up_after_endstop_or_probe_move();
    #if HOTENDS > 1
      tools.change(old_tool_index, 0, true);
    #endif
  }

  void print_signed_float(const char * const prefix, const float &f) {
    SERIAL_MSG("  ");
    SERIAL_PS(prefix);
    SERIAL_CHR(':');
    if (f >= 0) SERIAL_CHR('+');
    SERIAL_VAL(f, 2);
  }

  void print_G33_settings(const bool end_stops, const bool tower_angles) {
    SERIAL_MV(".Height:", mechanics.delta_height, 2);
    if (end_stops) {
      print_signed_float(PSTR("  Ex"), mechanics.delta_endstop_adj[A_AXIS]);
      print_signed_float(PSTR("Ey"), mechanics.delta_endstop_adj[B_AXIS]);
      print_signed_float(PSTR("Ez"), mechanics.delta_endstop_adj[C_AXIS]);
      SERIAL_MV("    Radius:", mechanics.delta_radius, 2);
    }
    SERIAL_EOL();
    if (tower_angles) {
      SERIAL_MSG(".Tower angle :  ");
      print_signed_float(PSTR("Tx"), mechanics.delta_tower_angle_adj[A_AXIS]);
      print_signed_float(PSTR("Ty"), mechanics.delta_tower_angle_adj[B_AXIS]);
      print_signed_float(PSTR("Tz"), mechanics.delta_tower_angle_adj[C_AXIS]);
      SERIAL_EMV("       Rod:", mechanics.delta_diagonal_rod, 2);
    }
  }

  /**
   * Delta AutoCalibration Algorithm based on Thinkyhead Marlin
   *       Calibrate height, endstops, delta radius, and tower angles.
   *
   * Parameters:
   *
   *   Pn Number of probe points:
   *
   *      P0     No probe. Normalize only.
   *      P1     Probe center and set height only.
   *      P2     Probe center and towers. Set height, endstops, and delta radius.
   *      P3     Probe all positions: center, towers and opposite towers. Set all.
   *      P4-P7  Probe all positions at different locations and average them.
   *
   *   T0  Don't calibrate tower angle corrections
   *
   *   Cn.nn Calibration precision; when omitted calibrates to maximum precision
   *
   *   Fn  Force to run at least n iterations and takes the best result
   *
   *   Vn Verbose level:
   *
   *      V0  Dry-run mode. Report settings and probe results. No calibration.
   *      V1  Report settings
   *      V2  Report settings and probe results
   *
   *   E   Engage the probe for each point
   */
  inline void gcode_G33(void) {

    const int8_t probe_points = parser.intval('P', 4);
    if (!WITHIN(probe_points, 0, 7)) {
      SERIAL_EM("?(P)oints is implausible (0-7).");
      return;
    }

    const int8_t verbose_level = parser.byteval('V', 1);
    if (!WITHIN(verbose_level, 0, 2)) {
      SERIAL_EM("?(V)erbose Level is implausible (0-2).");
      return;
    }

    const float calibration_precision = parser.floatval('C');
    if (calibration_precision < 0) {
      SERIAL_EM("?(C)alibration precision is implausible (>0).");
      return;
    }

    const int8_t force_iterations = parser.intval('F', 0);
    if (!WITHIN(force_iterations, 0, 30)) {
      SERIAL_EM("?(F)orce iteration is implausible (0-30).");
      return;
    }

    const bool  towers_set            = parser.boolval('T', true),
                stow_after_each       = parser.boolval('E'),
                _0p_calibration       = probe_points == 0,
                _1p_calibration       = probe_points == 1,
                _4p_calibration       = probe_points == 2,
                _4p_towers_points     = _4p_calibration && towers_set,
                _4p_opposite_points   = _4p_calibration && !towers_set,
                _7p_calibration       = probe_points >= 3 || _0p_calibration,
                _7p_half_circle       = probe_points == 3,
                _7p_double_circle     = probe_points == 5,
                _7p_triple_circle     = probe_points == 6,
                _7p_quadruple_circle  = probe_points == 7,
                _7p_multi_circle      = _7p_double_circle || _7p_triple_circle || _7p_quadruple_circle,
                _7p_intermed_points   = _7p_calibration && !_7p_half_circle;

    const static char save_message[] PROGMEM = "Save with M500 and/or copy to configuration_delta.h";

    int8_t iterations = 0;
    float test_precision,
          zero_std_dev = (verbose_level ? 999.0 : 0.0), // 0.0 in dry-run mode : forced end
          zero_std_dev_old = zero_std_dev,
          zero_std_dev_min = zero_std_dev,
          e_old[ABC] = {
            mechanics.delta_endstop_adj[A_AXIS],
            mechanics.delta_endstop_adj[B_AXIS],
            mechanics.delta_endstop_adj[C_AXIS]
          },
          dr_old = mechanics.delta_radius,
          dh_old = mechanics.delta_height,
          ta_old[ABC] = {
            mechanics.delta_tower_angle_adj[A_AXIS],
            mechanics.delta_tower_angle_adj[B_AXIS],
            mechanics.delta_tower_angle_adj[C_AXIS]
          };

    if (!_1p_calibration && !_0p_calibration) {  // test if the outer radius is reachable
      const float circles = (_7p_quadruple_circle ? 1.5 :
                             _7p_triple_circle    ? 1.0 :
                             _7p_double_circle    ? 0.5 : 0),
                  r = (1 + circles * 0.1) * mechanics.delta_probe_radius;
      for (uint8_t axis = 1; axis < 13; ++axis) {
        const float a = RADIANS(180 + 30 * axis);
        if (!mechanics.position_is_reachable_by_probe_xy(COS(a) * r + probe.offset[X_AXIS], SIN(a) * r + probe.offset[Y_AXIS])) {
          SERIAL_EM("?(M666 P)robe radius is implausible.");
          return;
        }
      }
    }

    SERIAL_EM("G33 Auto Calibrate");

    stepper.synchronize();

    #if HAS_LEVELING
      bedlevel.reset(); // After calibration bed-level data is no longer valid
    #endif

    #if HOTENDS > 1
      const uint8_t old_tool_index = tools.active_extruder;
      tools.change(0, 0, true);
      #define CALIBRATION_CLEANUP() Calibration_cleanup(old_tool_index)
    #else
      #define CALIBRATION_CLEANUP() Calibration_cleanup()
    #endif

    printer.setup_for_endstop_or_probe_move();
    endstops.enable(true);
    if (!_0p_calibration) {
      if (!mechanics.Home()) return;
      endstops.not_homing();
      DEPLOY_PROBE();
    }

    // print settings

    SERIAL_MSG(MSG_DELTA_CHECKING);
    if (verbose_level == 0) SERIAL_MSG(" (DRY-RUN)");
    SERIAL_EOL();
    LCD_MESSAGEPGM(MSG_DELTA_CHECKING);

    print_G33_settings(!_1p_calibration, _7p_calibration && towers_set);

    do {

      float z_at_pt[13] = { 0.0 };

      test_precision = zero_std_dev_old != 999.0 ? (zero_std_dev + zero_std_dev_old) / 2 : zero_std_dev;

      iterations++;

      // Probe the points

      if (!_0p_calibration) {
        if (!_7p_half_circle && !_7p_triple_circle) {   // probe center (P1=1, 2=1, 3=0, 4=1, 5=1, 6=0, 7=1)
          z_at_pt[0] += probe.check_pt(probe.offset[X_AXIS], probe.offset[Y_AXIS], stow_after_each, 1, false);
          if (isnan(z_at_pt[0])) return CALIBRATION_CLEANUP();
        }
        if (_7p_calibration) {                          // probe extra center points (P1=0, 2=0, 3=3, 4=3, 5=6, 6=6, 7=6)
          for (int8_t axis = _7p_multi_circle ? 11 : 9; axis > 0; axis -= _7p_multi_circle ? 2 : 4) {
            const float a = RADIANS(180 + 30 * axis), r = mechanics.delta_probe_radius * 0.1;
            z_at_pt[0] += probe.check_pt(COS(a) * r + probe.offset[X_AXIS], SIN(a) * r + probe.offset[Y_AXIS], stow_after_each, 1);
            if (isnan(z_at_pt[0])) return CALIBRATION_CLEANUP();
          }
          z_at_pt[0] /= float(_7p_double_circle ? 7 : probe_points);
        }
        if (!_1p_calibration) {                         // probe the radius points (P1=0, 2=3, 3=6, 4=12, 5=18, 6=30, 7=42)
          bool zig_zag = true;
          const uint8_t start = _4p_opposite_points ? 3 : 1,
                         step = _4p_calibration ? 4 : _7p_half_circle ? 2 : 1;
          for (uint8_t axis = start; axis < 13; axis += step) {
            const float zigadd = (zig_zag ? 0.5 : 0.0),
                        offset_circles =  _7p_quadruple_circle ? zigadd + 1.0 :
                                          _7p_triple_circle    ? zigadd + 0.5 :
                                          _7p_double_circle    ? zigadd : 0;
            for (float circles = -offset_circles ; circles <= offset_circles; circles++) {
              const float a = RADIANS(180 + 30 * axis),
                          r = mechanics.delta_probe_radius * (1 + circles * (zig_zag ? 0.1 : -0.1));
              z_at_pt[axis] += probe.check_pt(COS(a) * r + probe.offset[X_AXIS], SIN(a) * r + probe.offset[Y_AXIS], stow_after_each, 1);
              if (isnan(z_at_pt[axis])) return CALIBRATION_CLEANUP();
            }
            zig_zag = !zig_zag;
            z_at_pt[axis] /= (2 * offset_circles + 1);
          }
        }
        if (_7p_intermed_points) // average intermediates to tower and opposites
          for (uint8_t axis = 1; axis < 13; axis += 2)
          z_at_pt[axis] = (z_at_pt[axis] + (z_at_pt[axis + 1] + z_at_pt[(axis + 10) % 12 + 1]) / 2.0) / 2.0;
      }

      float S1  = z_at_pt[0],
            S2  = sq(z_at_pt[0]);
      int16_t N = 1;
      if (!_1p_calibration) { // std dev from zero plane
        for (uint8_t axis = (_4p_opposite_points ? 3 : 1); axis < 13; axis += (_4p_calibration ? 4 : 2)) {
          S1 += z_at_pt[axis];
          S2 += sq(z_at_pt[axis]);
          N++;
        }
      }
      zero_std_dev_old = zero_std_dev;
      zero_std_dev = round(SQRT(S2 / N) * 1000.0) / 1000.0 + 0.00001;

      // Solve matrices

      if ((zero_std_dev < test_precision || iterations <= force_iterations) && zero_std_dev > calibration_precision) {
        if (zero_std_dev < zero_std_dev_min) {
          COPY_ARRAY(e_old, mechanics.delta_endstop_adj);
          dr_old = mechanics.delta_radius;
          dh_old = mechanics.delta_height;
          COPY_ARRAY(ta_old, mechanics.delta_tower_angle_adj);
        }

        float e_delta[ABC] = { 0.0 }, r_delta = 0.0, t_delta[ABC] = { 0.0 };
        const float r_diff = mechanics.delta_radius - mechanics.delta_probe_radius,
                    h_factor = (1.00 + r_diff * 0.001) / 6.0,                                           // 1.02 for r_diff = 20mm
                    r_factor = (-(1.75 + 0.005 * r_diff + 0.001 * sq(r_diff))) / 6.0,                   // 2.25 for r_diff = 20mm
                    a_factor = (66.66 / mechanics.delta_probe_radius) / (iterations == 1 ? 16.0 : 2.0); // 0.83 for cal_rd = 80mm

        #define ZP(N,I) ((N) * z_at_pt[I])
        #define Z6(I) ZP(6, I)
        #define Z4(I) ZP(4, I)
        #define Z2(I) ZP(2, I)
        #define Z1(I) ZP(1, I)

        #if ENABLED(PROBE_MANUALLY)
          test_precision = 0.00; // forced end
        #endif

        switch (probe_points) {
          case 0:
            #if DISABLED(PROBE_MANUALLY)
              test_precision = 0.00; // forced end
            #endif
            break;

          case 1:
            #if DISABLED(PROBE_MANUALLY)
              test_precision = 0.00; // forced end
            #endif
            LOOP_XYZ(i) e_delta[i] = Z1(0);
            break;

          case 2:
            if (towers_set) {
              e_delta[A_AXIS] = (Z6(0) + Z4(1) - Z2(5) - Z2(9)) * h_factor;
              e_delta[B_AXIS] = (Z6(0) - Z2(1) + Z4(5) - Z2(9)) * h_factor;
              e_delta[C_AXIS] = (Z6(0) - Z2(1) - Z2(5) + Z4(9)) * h_factor;
              r_delta         = (Z6(0) - Z2(1) - Z2(5) - Z2(9)) * r_factor;
            }
            else {
              e_delta[A_AXIS] = (Z6(0) - Z4(7) + Z2(11) + Z2(3)) * h_factor;
              e_delta[B_AXIS] = (Z6(0) + Z2(7) - Z4(11) + Z2(3)) * h_factor;
              e_delta[C_AXIS] = (Z6(0) + Z2(7) + Z2(11) - Z4(3)) * h_factor;
              r_delta         = (Z6(0) - Z2(7) - Z2(11) - Z2(3)) * r_factor;
            }
            break;

          default:
            e_delta[A_AXIS] = (Z6(0) + Z2(1) - Z1(5) - Z1(9) - Z2(7) + Z1(11) + Z1(3)) * h_factor;
            e_delta[B_AXIS] = (Z6(0) - Z1(1) + Z2(5) - Z1(9) + Z1(7) - Z2(11) + Z1(3)) * h_factor;
            e_delta[C_AXIS] = (Z6(0) - Z1(1) - Z1(5) + Z2(9) + Z1(7) + Z1(11) - Z2(3)) * h_factor;
            r_delta         = (Z6(0) - Z1(1) - Z1(5) - Z1(9) - Z1(7) - Z1(11) - Z1(3)) * r_factor;

            if (towers_set) {
              t_delta[A_AXIS] = (            - Z2(5) + Z2(9)         - Z2(11) + Z2(3)) * a_factor;
              t_delta[B_AXIS] = (      Z2(1)         - Z2(9) + Z2(7)          - Z2(3)) * a_factor;
              t_delta[C_AXIS] = (    - Z2(1) + Z2(5)         - Z2(7) + Z2(11)        ) * a_factor;
              e_delta[A_AXIS] += (t_delta[B_AXIS] - t_delta[C_AXIS]) / 4.5;
              e_delta[B_AXIS] += (t_delta[C_AXIS] - t_delta[A_AXIS]) / 4.5;
              e_delta[C_AXIS] += (t_delta[A_AXIS] - t_delta[B_AXIS]) / 4.5;
            }
            break;
        }

        LOOP_XYZ(axis) mechanics.delta_endstop_adj[axis] += e_delta[axis];
        mechanics.delta_radius += r_delta;
        LOOP_XYZ(axis) mechanics.delta_tower_angle_adj[axis] += t_delta[axis];
      }
      else if (zero_std_dev >= test_precision) {   // step one back
        COPY_ARRAY(mechanics.delta_endstop_adj, e_old);
        mechanics.delta_radius = dr_old;
        mechanics.delta_height = dh_old;
        COPY_ARRAY(mechanics.delta_tower_angle_adj, ta_old);
      }
      if (verbose_level != 0) {                                    // !dry run
        // normalise angles to least squares
        float a_sum = 0.0;
        LOOP_XYZ(axis) a_sum += mechanics.delta_tower_angle_adj[axis];
        LOOP_XYZ(axis) mechanics.delta_tower_angle_adj[axis] -= a_sum / 3.0;

        // adjust delta_height and endstops by the max amount
        const float z_temp = MAX3(mechanics.delta_endstop_adj[A_AXIS], mechanics.delta_endstop_adj[B_AXIS], mechanics.delta_endstop_adj[C_AXIS]);
        mechanics.delta_height -= z_temp;
        LOOP_XYZ(i) mechanics.delta_endstop_adj[i] -= z_temp;
      }
      mechanics.recalc_delta_settings();
      NOMORE(zero_std_dev_min, zero_std_dev);

      // print report
      if (verbose_level != 1) {
        SERIAL_MSG(".    ");
        print_signed_float(PSTR("c"), z_at_pt[0]);
        if (_4p_towers_points || _7p_calibration) {
          print_signed_float(PSTR("   x"), z_at_pt[1]);
          print_signed_float(PSTR(" y"), z_at_pt[5]);
          print_signed_float(PSTR(" z"), z_at_pt[9]);
        }
        if (!_4p_opposite_points) SERIAL_EOL();
        if ((_4p_opposite_points) || _7p_calibration) {
          if (_7p_calibration) {
            SERIAL_CHR('.');
            SERIAL_SP(13);
          }
          print_signed_float(PSTR("  yz"), z_at_pt[7]);
          print_signed_float(PSTR("zx"), z_at_pt[11]);
          print_signed_float(PSTR("xy"), z_at_pt[3]);
          SERIAL_EOL();
        }
      }
      if (verbose_level != 0) {                                    // !dry run
        if ((zero_std_dev >= test_precision && iterations > force_iterations) || zero_std_dev <= calibration_precision) {  // end iterations
          SERIAL_MSG("Calibration OK");
          SERIAL_SP(36);
          #if DISABLED(PROBE_MANUALLY)
            if (zero_std_dev >= test_precision && !_1p_calibration)
              SERIAL_MSG("rolling back.");
            else
          #endif
            SERIAL_MV("std dev:", zero_std_dev, 3);
          SERIAL_EOL();
          char mess[21];
          sprintf_P(mess, PSTR("Calibration sd:"), "");
          if (zero_std_dev_min < 1)
            sprintf_P(&mess[15], PSTR("0.%03i"), (int)round(zero_std_dev_min * 1000.0));
          else
            sprintf_P(&mess[15], PSTR("%03i.x"), (int)round(zero_std_dev_min));
          lcd_setstatus(mess);
          print_G33_settings(!_1p_calibration, _7p_calibration && towers_set);
          SERIAL_PS(save_message);
          SERIAL_EOL();
        }
        else {                                                     // !end iterations
          char mess[15];
          if (iterations < 31)
            sprintf_P(mess, PSTR(".Iteration: %02i"), (int)iterations);
          else
            sprintf_P(mess, PSTR("No convergence"), "");
          SERIAL_TXT(mess);
          SERIAL_SP(36);
          SERIAL_EMV("std dev:", zero_std_dev, 3);
          lcd_setstatus(mess);
          print_G33_settings(!_1p_calibration, _7p_calibration && towers_set);
        }
      }
      else {
        const char *enddryrun = PSTR("End DRY-RUN");
        SERIAL_PS(enddryrun);
        SERIAL_SP(39);
        SERIAL_EMV("std dev:", zero_std_dev, 3);

        char mess[21];
        sprintf_P(mess, enddryrun, "");
        sprintf_P(&mess[11], PSTR(" sd:"), "");
        if (zero_std_dev < 1)
          sprintf_P(&mess[15], PSTR("0.%03i"), (int)round(zero_std_dev * 1000.0));
        else
          sprintf_P(&mess[15], PSTR("%03i.x"), (int)round(zero_std_dev));
        lcd_setstatus(mess);
      }

      endstops.enable(true);
      mechanics.Home();
      endstops.not_homing();

    } while (((zero_std_dev < test_precision && iterations < 31) || iterations <= force_iterations) && zero_std_dev > calibration_precision);

    CALIBRATION_CLEANUP();
  }

#endif // ENABLED(DELTA_AUTO_CALIBRATION_2)
