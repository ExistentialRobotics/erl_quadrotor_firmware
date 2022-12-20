/****************************************************************************
 *
 *   Copyright (c) 2013-2019 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/
/**
 * @file mission_feasibility_checker.cpp
 * Provides checks if mission is feasible given the navigation capabilities
 *
 * @author Lorenz Meier <lm@inf.ethz.ch>
 * @author Thomas Gubler <thomasgubler@student.ethz.ch>
 * @author Sander Smeets <sander@droneslab.com>
 * @author Nuno Marques <nuno.marques@dronesolutions.io>
 */

#include "mission_feasibility_checker.h"

#include "mission_block.h"
#include "navigator.h"

#include <drivers/drv_pwm_output.h>
#include <lib/geo/geo.h>
#include <lib/mathlib/mathlib.h>
#include <uORB/Subscription.hpp>
#include <px4_platform_common/events.h>

bool
MissionFeasibilityChecker::checkMissionFeasible(const mission_s &mission,
		float max_distance_to_1st_waypoint, float max_distance_between_waypoints)
{
	// Reset warning flag
	_navigator->get_mission_result()->warning = false;

	// trivial case: A mission with length zero cannot be valid
	if ((int)mission.count <= 0) {
		return false;
	}

	bool failed = false;

	// first check if we have a valid position
	const bool home_valid = _navigator->home_global_position_valid();
	const bool home_alt_valid = _navigator->home_alt_valid();

	if (!home_alt_valid) {
		failed = true;
		events::send(events::ID("navigator_mis_no_pos_lock"), events::Log::Info, "Not yet ready for mission, no position lock");

	} else {
		failed |= !checkDistanceToFirstWaypoint(mission, max_distance_to_1st_waypoint);
	}

	const float home_alt = _navigator->get_home_position()->alt;

	// reset for next check
	_has_takeoff = false;
	_has_landing = false;

	// run generic (for all vehicle types) checks
	failed |= !checkMissionItemValidity(mission);
	failed |= !checkDistancesBetweenWaypoints(mission, max_distance_between_waypoints);
	failed |= !checkGeofence(mission, home_alt, home_valid);
	failed |= !checkHomePositionAltitude(mission, home_alt, home_alt_valid);
	failed |= !checkTakeoff(mission, home_alt);

	// run type-specifc landing checks, which also include seting _has_landing that is used in checkTakeoffLandAvailable()
	if (_navigator->get_vstatus()->is_vtol) {
		failed |= !checkVTOLLanding(mission);

	} else if (_navigator->get_vstatus()->vehicle_type == vehicle_status_s::VEHICLE_TYPE_FIXED_WING) {
		failed |= !checkFixedWingLanding(mission);

	} else {
		// if neither VTOL nor FW, only check if mission has landing but don't check that one for validity
		_has_landing = hasMissionLanding(mission);
	}

	failed |= !checkTakeoffLandAvailable();

	return !failed;
}

bool
MissionFeasibilityChecker::checkGeofence(const mission_s &mission, float home_alt, bool home_valid)
{
	if (_navigator->get_geofence().isHomeRequired() && !home_valid) {
		events::send(events::ID("navigator_mis_geofence_no_home"), {events::Log::Error, events::LogInternal::Info},
			     "Geofence requires a valid home position");
		return false;
	}

	/* Check if all mission items are inside the geofence (if we have a valid geofence) */
	if (_navigator->get_geofence().valid()) {
		for (size_t i = 0; i < mission.count; i++) {
			struct mission_item_s missionitem = {};
			const ssize_t len = sizeof(missionitem);

			if (dm_read((dm_item_t)mission.dataman_id, i, &missionitem, len) != len) {
				/* not supposed to happen unless the datamanager can't access the SD card, etc. */
				return false;
			}

			if (missionitem.altitude_is_relative && !home_valid) {
				events::send(events::ID("navigator_mis_geofence_no_home2"), {events::Log::Error, events::LogInternal::Info},
					     "Geofence requires a valid home position");
				return false;
			}

			// Geofence function checks against home altitude amsl
			missionitem.altitude = missionitem.altitude_is_relative ? missionitem.altitude + home_alt : missionitem.altitude;

			if (MissionBlock::item_contains_position(missionitem) && !_navigator->get_geofence().check(missionitem)) {

				events::send<int16_t>(events::ID("navigator_mis_geofence_violation"), {events::Log::Error, events::LogInternal::Info},
						      "Geofence violation for waypoint {1}",
						      i + 1);
				return false;
			}
		}
	}

	return true;
}

bool
MissionFeasibilityChecker::checkHomePositionAltitude(const mission_s &mission, float home_alt, bool home_alt_valid)
{
	/* Check if all waypoints are above the home altitude */
	for (size_t i = 0; i < mission.count; i++) {
		struct mission_item_s missionitem = {};
		const ssize_t len = sizeof(struct mission_item_s);

		if (dm_read((dm_item_t)mission.dataman_id, i, &missionitem, len) != len) {
			_navigator->get_mission_result()->warning = true;
			/* not supposed to happen unless the datamanager can't access the SD card, etc. */
			return false;
		}

		/* reject relative alt without home set */
		if (missionitem.altitude_is_relative && !home_alt_valid && MissionBlock::item_contains_position(missionitem)) {

			_navigator->get_mission_result()->warning = true;

			events::send<int16_t>(events::ID("navigator_mis_no_home_rel_alt"), {events::Log::Error, events::LogInternal::Info},
					      "Mission rejected: No home position, waypoint {1} uses relative altitude",
					      i + 1);
			return false;

		}

		/* calculate the global waypoint altitude */
		float wp_alt = (missionitem.altitude_is_relative) ? missionitem.altitude + home_alt : missionitem.altitude;

		if (home_alt_valid && home_alt > wp_alt && MissionBlock::item_contains_position(missionitem)) {

			_navigator->get_mission_result()->warning = true;

			events::send<int16_t>(events::ID("navigator_mis_wp_below_home"), {events::Log::Warning, events::LogInternal::Info},
					      "Waypoint {1} below home", i + 1);
		}
	}

	return true;
}

bool
MissionFeasibilityChecker::checkMissionItemValidity(const mission_s &mission)
{
	// do not allow mission if we find unsupported item
	for (size_t i = 0; i < mission.count; i++) {
		struct mission_item_s missionitem;
		const ssize_t len = sizeof(struct mission_item_s);

		if (dm_read((dm_item_t)mission.dataman_id, i, &missionitem, len) != len) {
			// not supposed to happen unless the datamanager can't access the SD card, etc.
			events::send(events::ID("navigator_mis_sd_failure"), events::Log::Error,
				     "Mission rejected: Cannot access mission storage");
			return false;
		}

		// check if we find unsupported items and reject mission if so
		if (missionitem.nav_cmd != NAV_CMD_IDLE &&
		    missionitem.nav_cmd != NAV_CMD_WAYPOINT &&
		    missionitem.nav_cmd != NAV_CMD_LOITER_UNLIMITED &&
		    missionitem.nav_cmd != NAV_CMD_LOITER_TIME_LIMIT &&
		    missionitem.nav_cmd != NAV_CMD_RETURN_TO_LAUNCH &&
		    missionitem.nav_cmd != NAV_CMD_LAND &&
		    missionitem.nav_cmd != NAV_CMD_TAKEOFF &&
		    missionitem.nav_cmd != NAV_CMD_LOITER_TO_ALT &&
		    missionitem.nav_cmd != NAV_CMD_VTOL_TAKEOFF &&
		    missionitem.nav_cmd != NAV_CMD_VTOL_LAND &&
		    missionitem.nav_cmd != NAV_CMD_DELAY &&
		    missionitem.nav_cmd != NAV_CMD_CONDITION_GATE &&
		    missionitem.nav_cmd != NAV_CMD_DO_WINCH &&
		    missionitem.nav_cmd != NAV_CMD_DO_GRIPPER &&
		    missionitem.nav_cmd != NAV_CMD_DO_JUMP &&
		    missionitem.nav_cmd != NAV_CMD_DO_CHANGE_SPEED &&
		    missionitem.nav_cmd != NAV_CMD_DO_SET_HOME &&
		    missionitem.nav_cmd != NAV_CMD_DO_SET_SERVO &&
		    missionitem.nav_cmd != NAV_CMD_DO_SET_ACTUATOR &&
		    missionitem.nav_cmd != NAV_CMD_DO_LAND_START &&
		    missionitem.nav_cmd != NAV_CMD_DO_TRIGGER_CONTROL &&
		    missionitem.nav_cmd != NAV_CMD_DO_DIGICAM_CONTROL &&
		    missionitem.nav_cmd != NAV_CMD_IMAGE_START_CAPTURE &&
		    missionitem.nav_cmd != NAV_CMD_IMAGE_STOP_CAPTURE &&
		    missionitem.nav_cmd != NAV_CMD_VIDEO_START_CAPTURE &&
		    missionitem.nav_cmd != NAV_CMD_VIDEO_STOP_CAPTURE &&
		    missionitem.nav_cmd != NAV_CMD_DO_CONTROL_VIDEO &&
		    missionitem.nav_cmd != NAV_CMD_DO_MOUNT_CONFIGURE &&
		    missionitem.nav_cmd != NAV_CMD_DO_MOUNT_CONTROL &&
		    missionitem.nav_cmd != NAV_CMD_DO_GIMBAL_MANAGER_PITCHYAW &&
		    missionitem.nav_cmd != NAV_CMD_DO_GIMBAL_MANAGER_CONFIGURE &&
		    missionitem.nav_cmd != NAV_CMD_DO_SET_ROI &&
		    missionitem.nav_cmd != NAV_CMD_DO_SET_ROI_LOCATION &&
		    missionitem.nav_cmd != NAV_CMD_DO_SET_ROI_WPNEXT_OFFSET &&
		    missionitem.nav_cmd != NAV_CMD_DO_SET_ROI_NONE &&
		    missionitem.nav_cmd != NAV_CMD_DO_SET_CAM_TRIGG_DIST &&
		    missionitem.nav_cmd != NAV_CMD_OBLIQUE_SURVEY &&
		    missionitem.nav_cmd != NAV_CMD_DO_SET_CAM_TRIGG_INTERVAL &&
		    missionitem.nav_cmd != NAV_CMD_SET_CAMERA_MODE &&
		    missionitem.nav_cmd != NAV_CMD_SET_CAMERA_ZOOM &&
		    missionitem.nav_cmd != NAV_CMD_SET_CAMERA_FOCUS &&
		    missionitem.nav_cmd != NAV_CMD_DO_VTOL_TRANSITION) {

			events::send<uint16_t, uint16_t>(events::ID("navigator_mis_unsup_cmd"), {events::Log::Error, events::LogInternal::Warning},
							 "Mission rejected: item {1}: unsupported command: {2}", i + 1, missionitem.nav_cmd);
			return false;
		}

		/* Check non navigation item */
		if (missionitem.nav_cmd == NAV_CMD_DO_SET_SERVO) {

			/* check actuator number */
			if (missionitem.params[0] < 0 || missionitem.params[0] > 5) {
				events::send<uint32_t>(events::ID("navigator_mis_act_index"), {events::Log::Error, events::LogInternal::Warning},
						       "Actuator number {1} is out of bounds 0..5", (int)missionitem.params[0]);
				return false;
			}

			/* check actuator value */
			if (missionitem.params[1] < -PWM_DEFAULT_MAX || missionitem.params[1] > PWM_DEFAULT_MAX) {
				events::send<uint32_t, uint32_t>(events::ID("navigator_mis_act_range"), {events::Log::Error, events::LogInternal::Warning},
								 "Actuator value {1} is out of bounds -{2}..{2}", (int)missionitem.params[1], PWM_DEFAULT_MAX);
				return false;
			}
		}

		// check if the mission starts with a land command while the vehicle is landed
		if ((i == 0) && missionitem.nav_cmd == NAV_CMD_LAND && _navigator->get_land_detected()->landed) {

			events::send(events::ID("navigator_mis_starts_w_landing"), {events::Log::Error, events::LogInternal::Info},
				     "Mission rejected: starts with landing");
			return false;
		}
	}

	return true;
}

bool
MissionFeasibilityChecker::checkTakeoff(const mission_s &mission, float home_alt)
{
	bool takeoff_first = false;
	int takeoff_index = -1;

	for (size_t i = 0; i < mission.count; i++) {
		struct mission_item_s missionitem = {};
		const ssize_t len = sizeof(struct mission_item_s);

		if (dm_read((dm_item_t)mission.dataman_id, i, &missionitem, len) != len) {
			/* not supposed to happen unless the datamanager can't access the SD card, etc. */
			return false;
		}

		// look for a takeoff waypoint
		if (missionitem.nav_cmd == NAV_CMD_TAKEOFF || missionitem.nav_cmd == NAV_CMD_VTOL_TAKEOFF) {
			// make sure that the altitude of the waypoint is at least one meter larger than the acceptance radius
			// this makes sure that the takeoff waypoint is not reached before we are at least one meter in the air

			float takeoff_alt = missionitem.altitude_is_relative
					    ? missionitem.altitude
					    : missionitem.altitude - home_alt;

			// check if we should use default acceptance radius
			float acceptance_radius = _navigator->get_default_acceptance_radius();

			if (missionitem.acceptance_radius > NAV_EPSILON_POSITION) {
				acceptance_radius = missionitem.acceptance_radius;
			}

			if (takeoff_alt - 1.0f < acceptance_radius) {
				/* EVENT
				 * @description The minimum takeoff altitude is the acceptance radius plus 1m.
				 */
				events::send<float>(events::ID("navigator_mis_takeoff_too_low"), {events::Log::Error, events::LogInternal::Info},
						    "Mission rejected: takeoff altitude too low! Minimum: {1:.1m_v}", acceptance_radius + 1.f);
				return false;
			}

			// tell that mission has a takeoff waypoint
			_has_takeoff = true;

			// tell that a takeoff waypoint is the first "waypoint"
			// mission item
			if (i == 0) {
				takeoff_first = true;

			} else if (takeoff_index == -1) {
				// stores the index of the first takeoff waypoint
				takeoff_index = i;
			}
		}
	}

	if (takeoff_index != -1) {
		// checks if all the mission items before the first takeoff waypoint
		// are not waypoints or position-related items;
		// this means that, before a takeoff waypoint, one can set
		// one of the bellow mission items
		for (size_t i = 0; i < (size_t)takeoff_index; i++) {
			struct mission_item_s missionitem = {};
			const ssize_t len = sizeof(struct mission_item_s);

			if (dm_read((dm_item_t)mission.dataman_id, i, &missionitem, len) != len) {
				/* not supposed to happen unless the datamanager can't access the SD card, etc. */
				return false;
			}

			takeoff_first = !(missionitem.nav_cmd != NAV_CMD_IDLE &&
					  missionitem.nav_cmd != NAV_CMD_DELAY &&
					  missionitem.nav_cmd != NAV_CMD_DO_JUMP &&
					  missionitem.nav_cmd != NAV_CMD_DO_CHANGE_SPEED &&
					  missionitem.nav_cmd != NAV_CMD_DO_SET_HOME &&
					  missionitem.nav_cmd != NAV_CMD_DO_SET_SERVO &&
					  missionitem.nav_cmd != NAV_CMD_DO_LAND_START &&
					  missionitem.nav_cmd != NAV_CMD_DO_TRIGGER_CONTROL &&
					  missionitem.nav_cmd != NAV_CMD_DO_DIGICAM_CONTROL &&
					  missionitem.nav_cmd != NAV_CMD_IMAGE_START_CAPTURE &&
					  missionitem.nav_cmd != NAV_CMD_IMAGE_STOP_CAPTURE &&
					  missionitem.nav_cmd != NAV_CMD_VIDEO_START_CAPTURE &&
					  missionitem.nav_cmd != NAV_CMD_VIDEO_STOP_CAPTURE &&
					  missionitem.nav_cmd != NAV_CMD_DO_CONTROL_VIDEO &&
					  missionitem.nav_cmd != NAV_CMD_DO_MOUNT_CONFIGURE &&
					  missionitem.nav_cmd != NAV_CMD_DO_MOUNT_CONTROL &&
					  missionitem.nav_cmd != NAV_CMD_DO_GIMBAL_MANAGER_PITCHYAW &&
					  missionitem.nav_cmd != NAV_CMD_DO_GIMBAL_MANAGER_CONFIGURE &&
					  missionitem.nav_cmd != NAV_CMD_DO_SET_ROI &&
					  missionitem.nav_cmd != NAV_CMD_DO_SET_ROI_LOCATION &&
					  missionitem.nav_cmd != NAV_CMD_DO_SET_ROI_WPNEXT_OFFSET &&
					  missionitem.nav_cmd != NAV_CMD_DO_SET_ROI_NONE &&
					  missionitem.nav_cmd != NAV_CMD_DO_SET_CAM_TRIGG_DIST &&
					  missionitem.nav_cmd != NAV_CMD_OBLIQUE_SURVEY &&
					  missionitem.nav_cmd != NAV_CMD_DO_SET_CAM_TRIGG_INTERVAL &&
					  missionitem.nav_cmd != NAV_CMD_SET_CAMERA_MODE &&
					  missionitem.nav_cmd != NAV_CMD_SET_CAMERA_ZOOM &&
					  missionitem.nav_cmd != NAV_CMD_SET_CAMERA_FOCUS &&
					  missionitem.nav_cmd != NAV_CMD_DO_VTOL_TRANSITION);
		}
	}

	if (_has_takeoff && !takeoff_first) {
		// check if the takeoff waypoint is the first waypoint item on the mission
		// i.e, an item with position/attitude change modification
		// if it is not, the mission should be rejected
		events::send(events::ID("navigator_mis_takeoff_not_first"), {events::Log::Error, events::LogInternal::Info},
			     "Mission rejected: takeoff is not the first waypoint item");
		return false;
	}

	// all checks have passed
	return true;
}

bool
MissionFeasibilityChecker::hasMissionLanding(const mission_s &mission)
{
	// Go through all mission items and search for a landing waypoint.
	// For MC we currently do not run any checks on the validity of the planned landing.

	bool mission_landing_found = false;

	for (size_t i = 0; i < mission.count; i++) {
		struct mission_item_s missionitem;
		const ssize_t len = sizeof(missionitem);

		if (dm_read((dm_item_t)mission.dataman_id, i, &missionitem, len) != len) {
			/* not supposed to happen unless the datamanager can't access the SD card, etc. */
			return false;
		}

		if (missionitem.nav_cmd == NAV_CMD_LAND) {
			mission_landing_found = true;
		}
	}

	return mission_landing_found;
}

bool
MissionFeasibilityChecker::checkFixedWingLanding(const mission_s &mission)
{
	/* Go through all mission items and search for a landing waypoint
	 * if landing waypoint is found: the previous waypoint is checked to be at a feasible distance and altitude given the landing slope */

	bool landing_valid = false;

	size_t do_land_start_index = 0;
	size_t landing_approach_index = 0;

	for (size_t i = 0; i < mission.count; i++) {
		struct mission_item_s missionitem;
		const ssize_t len = sizeof(missionitem);

		if (dm_read((dm_item_t)mission.dataman_id, i, &missionitem, len) != len) {
			/* not supposed to happen unless the datamanager can't access the SD card, etc. */
			return false;
		}

		// if DO_LAND_START found then require valid landing AFTER
		if (missionitem.nav_cmd == NAV_CMD_DO_LAND_START) {
			if (_has_landing) {
				events::send(events::ID("navigator_mis_multiple_land"), {events::Log::Error, events::LogInternal::Info},
					     "Mission rejected: more than one land start commands");
				return false;

			} else {
				_has_landing = true;
				do_land_start_index = i;
			}
		}

		if (missionitem.nav_cmd == NAV_CMD_LAND) {
			mission_item_s missionitem_previous {};

			_has_landing = true;

			float param_fw_lnd_ang = 0.0f;
			const param_t param_handle_fw_lnd_ang = param_find("FW_LND_ANG");

			if (param_handle_fw_lnd_ang == PARAM_INVALID) {
				events::send(events::ID("navigator_mis_land_angle_param_missing"), {events::Log::Error, events::LogInternal::Info},
					     "Mission rejected: FW_LND_ANG parameter is missing");
				return false;

			} else {
				param_get(param_handle_fw_lnd_ang, &param_fw_lnd_ang);
			}

			if (i > 0) {
				landing_approach_index = i - 1;

				if (dm_read((dm_item_t)mission.dataman_id, landing_approach_index, &missionitem_previous, len) != len) {
					/* not supposed to happen unless the datamanager can't access the SD card, etc. */
					return false;
				}

				if (MissionBlock::item_contains_position(missionitem_previous)) {

					const float land_alt_amsl = missionitem.altitude_is_relative ? missionitem.altitude +
								    _navigator->get_home_position()->alt : missionitem.altitude;
					const float entrance_alt_amsl = missionitem_previous.altitude_is_relative ? missionitem_previous.altitude +
									_navigator->get_home_position()->alt : missionitem_previous.altitude;
					const float relative_approach_altitude = entrance_alt_amsl - land_alt_amsl;

					if (relative_approach_altitude < FLT_EPSILON) {
						events::send(events::ID("navigator_mis_approach_wp_below_land"), {events::Log::Error, events::LogInternal::Info},
							     "Mission rejected: the approach waypoint must be above the landing point");
						return false;
					}

					float landing_approach_distance;

					if (missionitem_previous.nav_cmd == NAV_CMD_LOITER_TO_ALT) {
						// assume this is a fixed-wing landing pattern with orbit to alt followed
						// by tangent exit to landing approach and touchdown at landing waypoint

						const float distance_orbit_center_to_land = get_distance_to_next_waypoint(missionitem_previous.lat,
								missionitem_previous.lon, missionitem.lat, missionitem.lon);
						const float orbit_radius = fabsf(missionitem_previous.loiter_radius);

						if (distance_orbit_center_to_land <= orbit_radius) {
							events::send(events::ID("navigator_mis_land_wp_inside_orbit_radius"), {events::Log::Error, events::LogInternal::Info},
								     "Mission rejected: the landing point must be outside the orbit radius");
							return false;
						}

						landing_approach_distance = sqrtf(distance_orbit_center_to_land * distance_orbit_center_to_land - orbit_radius *
										  orbit_radius);

					} else if (missionitem_previous.nav_cmd == NAV_CMD_WAYPOINT) {
						// approaching directly from waypoint position

						const float waypoint_distance = get_distance_to_next_waypoint(missionitem_previous.lat, missionitem_previous.lon,
										missionitem.lat, missionitem.lon);
						landing_approach_distance = waypoint_distance;

					} else {
						events::send(events::ID("navigator_mis_unsupported_landing_approach_wp"), {events::Log::Error, events::LogInternal::Info},
							     "Mission rejected: unsupported landing approach entrance waypoint type. Only ORBIT_TO_ALT or WAYPOINT allowed");
						return false;
					}

					const float glide_slope = relative_approach_altitude / landing_approach_distance;

					// respect user setting as max glide slope, but account for floating point
					// rounding on next check with small (arbitrary) 0.1 deg buffer, as the
					// landing angle parameter is what is typically used for steepest glide
					// in landing config
					const float max_glide_slope = tanf(math::radians(param_fw_lnd_ang + 0.1f));

					if (glide_slope > max_glide_slope) {

						const uint8_t land_angle_left_of_decimal = (uint8_t)param_fw_lnd_ang;
						const uint8_t land_angle_first_after_decimal = (uint8_t)((param_fw_lnd_ang - floorf(
									param_fw_lnd_ang)) * 10.0f);

						events::send<uint8_t, uint8_t>(events::ID("navigator_mis_glide_slope_too_steep"), {events::Log::Error, events::LogInternal::Info},
									       "Mission rejected: the landing glide slope is steeper than the vehicle setting of {1}.{2} degrees",
									       land_angle_left_of_decimal, land_angle_first_after_decimal);

						const uint32_t acceptable_entrance_alt = (uint32_t)(max_glide_slope * landing_approach_distance);
						const uint32_t acceptable_landing_dist = (uint32_t)ceilf(relative_approach_altitude / max_glide_slope);

						events::send<uint32_t, uint32_t>(events::ID("navigator_mis_correct_glide_slope"), {events::Log::Error, events::LogInternal::Info},
										 "Reduce the glide slope, lower the entrance altitude {1} meters, or increase the landing approach distance {2} meters",
										 acceptable_entrance_alt, acceptable_landing_dist);

						return false;
					}

					landing_valid = true;

				} else {
					// mission item before land doesn't have a position
					events::send(events::ID("navigator_mis_req_landing_approach"), {events::Log::Error, events::LogInternal::Info},
						     "Mission rejected: landing approach is required");
					return false;
				}

			} else {
				events::send(events::ID("navigator_mis_starts_w_landing2"), {events::Log::Error, events::LogInternal::Info},
					     "Mission rejected: starts with landing");
				return false;
			}

		} else if (missionitem.nav_cmd == NAV_CMD_RETURN_TO_LAUNCH) {
			if (_has_landing && do_land_start_index < i) {
				events::send(events::ID("navigator_mis_land_before_rtl"), {events::Log::Error, events::LogInternal::Info},
					     "Mission rejected: land start item before RTL item is not possible");
				return false;
			}
		}
	}

	if (_has_landing && (!landing_valid || (do_land_start_index > landing_approach_index))) {
		events::send(events::ID("navigator_mis_invalid_land"), {events::Log::Error, events::LogInternal::Info},
			     "Mission rejected: invalid land start");
		return false;
	}

	/* No landing waypoints or no waypoints */
	return true;
}

bool
MissionFeasibilityChecker::checkVTOLLanding(const mission_s &mission)
{
	// Go through all mission items and search for a landing waypoint, then run some checks on it

	size_t do_land_start_index = 0;
	size_t landing_approach_index = 0;

	for (size_t i = 0; i < mission.count; i++) {
		struct mission_item_s missionitem;
		const ssize_t len = sizeof(missionitem);

		if (dm_read((dm_item_t)mission.dataman_id, i, &missionitem, len) != len) {
			/* not supposed to happen unless the datamanager can't access the SD card, etc. */
			return false;
		}

		// if DO_LAND_START found then require valid landing AFTER
		if (missionitem.nav_cmd == NAV_CMD_DO_LAND_START) {
			if (_has_landing) {
				events::send(events::ID("navigator_mis_multi_land"), {events::Log::Error, events::LogInternal::Info},
					     "Mission rejected: more than one land start commands");
				return false;

			} else {
				_has_landing = true;
				do_land_start_index = i;
			}
		}

		if (missionitem.nav_cmd == NAV_CMD_LAND || missionitem.nav_cmd == NAV_CMD_VTOL_LAND) {
			mission_item_s missionitem_previous {};

			_has_landing = true;

			if (i > 0) {
				landing_approach_index = i - 1;

				if (dm_read((dm_item_t)mission.dataman_id, landing_approach_index, &missionitem_previous, len) != len) {
					/* not supposed to happen unless the datamanager can't access the SD card, etc. */
					return false;
				}


			} else {
				events::send(events::ID("navigator_mis_starts_w_land"), {events::Log::Error, events::LogInternal::Info},
					     "Mission rejected: starts with land waypoint");
				return false;
			}

		} else if (missionitem.nav_cmd == NAV_CMD_RETURN_TO_LAUNCH) {
			if (_has_landing && do_land_start_index < i) {
				events::send(events::ID("navigator_mis_land_before_rtl2"), {events::Log::Error, events::LogInternal::Info},
					     "Mission rejected: land start item before RTL item is not possible");
				return false;
			}
		}
	}

	if (_has_landing && (do_land_start_index > landing_approach_index)) {
		events::send(events::ID("navigator_mis_invalid_land2"), {events::Log::Error, events::LogInternal::Info},
			     "Mission rejected: invalid land start");
		return false;
	}

	/* No landing waypoints or no waypoints */
	return true;
}

bool
MissionFeasibilityChecker::checkTakeoffLandAvailable()
{
	bool result = true;

	switch (_navigator->get_takeoff_land_required()) {
	case 0:
		result = true;
		break;

	case 1:
		result = _has_takeoff;

		if (!result) {
			events::send(events::ID("navigator_mis_takeoff_missing"), {events::Log::Error, events::LogInternal::Info},
				     "Mission rejected: Takeoff waypoint required");
			return false;
		}

		break;

	case 2:
		result = _has_landing;

		if (!result) {
			events::send(events::ID("navigator_mis_land_missing"), {events::Log::Error, events::LogInternal::Info},
				     "Mission rejected: Landing waypoint/pattern required");
		}

		break;

	case 3:
		result = _has_takeoff && _has_landing;

		if (!result) {
			events::send(events::ID("navigator_mis_takeoff_or_land_missing"), {events::Log::Error, events::LogInternal::Info},
				     "Mission rejected: Takeoff or Landing item missing");
		}

		break;

	case 4:
		result = _has_takeoff == _has_landing;

		if (!result && (_has_takeoff)) {
			events::send(events::ID("navigator_mis_add_land_or_rm_to"), {events::Log::Error, events::LogInternal::Info},
				     "Mission rejected: Add Landing item or remove Takeoff");

		} else if (!result && (_has_landing)) {
			events::send(events::ID("navigator_mis_add_to_or_rm_land"), {events::Log::Error, events::LogInternal::Info},
				     "Mission rejected: Add Takeoff item or remove Landing");
		}

		break;

	default:
		result = true;
		break;
	}

	return result;
}

bool
MissionFeasibilityChecker::checkDistanceToFirstWaypoint(const mission_s &mission, float max_distance)
{
	if (max_distance <= 0.0f) {
		/* param not set, check is ok */
		return true;
	}

	/* find first waypoint (with lat/lon) item in datamanager */
	for (size_t i = 0; i < mission.count; i++) {

		struct mission_item_s mission_item {};

		if (!(dm_read((dm_item_t)mission.dataman_id, i, &mission_item, sizeof(mission_item_s)) == sizeof(mission_item_s))) {
			/* error reading, mission is invalid */
			events::send(events::ID("navigator_mis_storage_failure"), events::Log::Error,
				     "Error reading mission storage");
			return false;
		}

		/* check only items with valid lat/lon */
		if (!MissionBlock::item_contains_position(mission_item)) {
			continue;
		}

		/* check distance from current position to item */
		float dist_to_1wp = get_distance_to_next_waypoint(
					    mission_item.lat, mission_item.lon,
					    _navigator->get_home_position()->lat, _navigator->get_home_position()->lon);

		if (dist_to_1wp < max_distance) {

			return true;

		} else {
			/* item is too far from home */
			events::send<uint32_t, uint32_t>(events::ID("navigator_mis_first_wp_too_far"), {events::Log::Error, events::LogInternal::Info},
							 "First waypoint too far away: {1m} (maximum: {2m})", (uint32_t)dist_to_1wp, (uint32_t)max_distance);

			_navigator->get_mission_result()->warning = true;
			return false;
		}
	}

	/* no waypoints found in mission, then we will not fly far away */
	return true;
}

bool
MissionFeasibilityChecker::checkDistancesBetweenWaypoints(const mission_s &mission, float max_distance)
{
	if (max_distance <= 0.0f) {
		/* param not set, check is ok */
		return true;
	}

	double last_lat = (double)NAN;
	double last_lon = (double)NAN;
	int last_cmd = 0;

	/* Go through all waypoints */
	for (size_t i = 0; i < mission.count; i++) {

		struct mission_item_s mission_item {};

		if (!(dm_read((dm_item_t)mission.dataman_id, i, &mission_item, sizeof(mission_item_s)) == sizeof(mission_item_s))) {
			/* error reading, mission is invalid */
			events::send(events::ID("navigator_mis_storage_failure2"), events::Log::Error,
				     "Error reading mission storage");
			return false;
		}

		/* check only items with valid lat/lon */
		if (!MissionBlock::item_contains_position(mission_item)) {
			continue;
		}

		/* Compare it to last waypoint if already available. */
		if (PX4_ISFINITE(last_lat) && PX4_ISFINITE(last_lon)) {

			/* check distance from current position to item */
			const float dist_between_waypoints = get_distance_to_next_waypoint(
					mission_item.lat, mission_item.lon,
					last_lat, last_lon);


			if (dist_between_waypoints > max_distance) {
				/* distance between waypoints is too high */
				events::send<uint32_t, uint32_t>(events::ID("navigator_mis_wp_dist_too_far"), {events::Log::Error, events::LogInternal::Info},
								 "Distance between waypoints too far: {1m}, (maximum: {2m})", (uint32_t)dist_between_waypoints, (uint32_t)max_distance);

				_navigator->get_mission_result()->warning = true;
				return false;

				/* do not allow waypoints that are literally on top of each other */

				/* and do not allow condition gates that are at the same position as a navigation waypoint */

			} else if (dist_between_waypoints < 0.05f &&
				   (mission_item.nav_cmd == NAV_CMD_CONDITION_GATE || last_cmd == NAV_CMD_CONDITION_GATE)) {

				/* Waypoints and gate are at the exact same position, which indicates an
				 * invalid mission and makes calculating the direction from one waypoint
				 * to another impossible. */
				events::send<float, float>(events::ID("navigator_mis_wp_gate_too_close"), {events::Log::Error, events::LogInternal::Info},
							   "Distance between waypoint and gate too close: {1:.3m} (minimum: {2:.3m})", dist_between_waypoints, 0.05f);

				_navigator->get_mission_result()->warning = true;
				return false;
			}
		}

		last_lat = mission_item.lat;
		last_lon = mission_item.lon;
		last_cmd = mission_item.nav_cmd;
	}

	/* We ran through all waypoints and have not found any distances between waypoints that are too far. */
	return true;
}
