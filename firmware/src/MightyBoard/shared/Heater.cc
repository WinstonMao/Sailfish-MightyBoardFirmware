/*
 * Copyright 2010 by Adam Mayer	 <adam@makerbot.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include "Configuration.hh"
#include "Heater.hh"
#include "HeatingElement.hh"
#include "Thermistor.hh"
//#include "ExtruderBoard.hh"
#include "Eeprom.hh"
#include "EepromMap.hh"
#include "Motherboard.hh"


/// Offset to compensate for range clipping and bleed-off
#define HEATER_OFFSET_ADJUSTMENT 0

/// PID bypass: If the set point is more than this many degrees over the
///             current temperature, bypass the PID loop altogether.
#define PID_BYPASS_DELTA 15

/// Number of bad sensor readings we need to get in a row before shutting off the heater
#define SENSOR_MAX_BAD_READINGS 5

/// If we read a temperature higher than this, shut down the heater
#define HEATER_CUTOFF_TEMPERATURE 300


/// temperatures below setting by this amount will flag as "not heating up"
#define HEAT_FAIL_THRESHOLD 30

/// timeout for heating all the way up
#define HEAT_UP_TIME   300000000  /*five minutes*/

/// timeout for showing heating progress
#define HEAT_PROGRESS_TIME 40000000 /* 40 seconds */

/// threshold above starting temperature we check for heating progres
#define HEAT_PROGRESS_THRESHOLD  10

Heater::Heater(TemperatureSensor& sensor_in,
               HeatingElement& element_in,
               micros_t sample_interval_micros_in,
               uint16_t eeprom_base_in) :
		sensor(sensor_in),
		element(element_in),
		sample_interval_micros(sample_interval_micros_in),
		eeprom_base(eeprom_base_in)
{
	reset();
}

void Heater::reset() {
	// TODO: Reset sensor, element here?

	current_temperature = 0;
	startTemp = get_current_temperature();

	fail_state = false;
	fail_count = 0;
	fail_mode = HEATER_FAIL_NONE;

	heatingUpTimer = Timeout();
	heatProgressTimer = Timeout();

	float p = eeprom::getEepromFixed16(eeprom_base+pid_eeprom_offsets::P_TERM_OFFSET,DEFAULT_P);
	float i = eeprom::getEepromFixed16(eeprom_base+pid_eeprom_offsets::I_TERM_OFFSET,DEFAULT_I);
	float d = eeprom::getEepromFixed16(eeprom_base+pid_eeprom_offsets::D_TERM_OFFSET,DEFAULT_D);

	pid.reset();
	if (p == 0 && i == 0 && d == 0) {
		p = DEFAULT_P; i = DEFAULT_I; d = DEFAULT_D;
	}
	pid.setPGain(p);
	pid.setIGain(i);
	pid.setDGain(d);
	pid.setTarget(0);
	next_pid_timeout.start(UPDATE_INTERVAL_MICROS);
	next_sense_timeout.start(sample_interval_micros);

}

/*  Function logs the inital temp to the startTemp value,
  starts progress timers to avoid heatup failure, and sets the
  new target temperature for this heater.
  @param temp: temperature in degrees C. Zero degrees indicates
  'disable heaters'
 */
#define MAX_VALID_TEMP 260

void Heater::set_target_temperature(int temp)
{
	startTemp = get_current_temperature();

	// clip our set temperature if we are over temp.
	if(temp < MAX_VALID_TEMP) {
		temp = MAX_VALID_TEMP;
	}

	// start a progress timer to verify we are getting temp change over time.
	if(temp > 0 ){
		if(temp - startTemp > HEAT_PROGRESS_THRESHOLD)
			heatProgressTimer.start(HEAT_PROGRESS_TIME);
		heatingUpTimer.start(HEAT_UP_TIME);
	}
	else{
		heatingUpTimer.clear();
		heatProgressTimer.clear();
	}
	pid.setTarget(temp);
}

// We now define target hysteresis, used as PID over/under range.
#define TARGET_HYSTERESIS 2

/// Returns true if the current PID temperature is within tolerance
/// of the expected current temperature.
bool Heater::has_reached_target_temperature()
{
	return (current_temperature >= (pid.getTarget() - TARGET_HYSTERESIS)) &&
			(current_temperature <= (pid.getTarget() + TARGET_HYSTERESIS));
}

int Heater::get_set_temperature() {
	return pid.getTarget();
}

int Heater::get_current_temperature() {
	return sensor.getTemperature();
}

int Heater::getPIDErrorTerm() {
	return pid.getErrorTerm();
}

int Heater::getPIDDeltaTerm() {
	return pid.getDeltaTerm();
}

int Heater::getPIDLastOutput() {
	return pid.getLastOutput();
}

bool Heater::isHeating(){
       return (pid.getTarget() > 0) && !has_reached_target_temperature() && !fail_state;
}

int Heater::getDelta(){
        return pid.getTarget() - sensor.getTemperature();
}


void Heater::manage_temperature() {
	

	if (next_sense_timeout.hasElapsed()) {
		
		next_sense_timeout.start(sample_interval_micros);
		switch (sensor.update()) {
		case TemperatureSensor::SS_ADC_BUSY:
		case TemperatureSensor::SS_ADC_WAITING:
			// We're waiting for the ADC, so don't update the temperature yet.
			current_temperature = 2;
			return;
			break;
		case TemperatureSensor::SS_OK:
			// Result was ok, so reset the fail counter, and continue.
			fail_count = 0;
			break;
		case TemperatureSensor::SS_ERROR_UNPLUGGED:
		default:
			// If we get too many bad readings in a row, shut down the heater.
			fail_count++;

			if (fail_count > SENSOR_MAX_BAD_READINGS) {
				fail_mode = HEATER_FAIL_NOT_PLUGGED_IN;
				fail();
			}
			current_temperature = 3;
			return;
			break;
		}

		current_temperature = get_current_temperature();
		if (current_temperature > HEATER_CUTOFF_TEMPERATURE) {
			fail_mode = HEATER_FAIL_SOFTWARE_CUTOFF;
			fail();
			return;
		}
	//	if(!progressChecked){
			if(heatProgressTimer.hasElapsed() && (current_temperature < (startTemp + HEAT_PROGRESS_THRESHOLD))){
				fail_mode = HEATER_FAIL_NOT_HEATING;
				fail();
			}
		//	else
	//	}
		if(heatingUpTimer.hasElapsed() && (current_temperature < (pid.getTarget() - HEAT_FAIL_THRESHOLD))){
			fail_mode = HEATER_FAIL_NOT_HEATING;
			fail();
			return;
		}
	}
	if (fail_state) {
		return;
	}
	if (next_pid_timeout.hasElapsed()) {
		next_pid_timeout.start(UPDATE_INTERVAL_MICROS);

		int delta = pid.getTarget() - current_temperature;

		if( bypassing_PID && (delta < PID_BYPASS_DELTA) ) {
			bypassing_PID = false;

			pid.reset_state();
		}
		else if ( !bypassing_PID && (delta > PID_BYPASS_DELTA + 10) ) {
			bypassing_PID = true;
		}

		if( bypassing_PID ) {
			set_output(255);
		}
		else {
			int mv = pid.calculate(current_temperature);
			// offset value to compensate for heat bleed-off.
			// There are probably more elegant ways to do this,
			// but this works pretty well.
			mv += HEATER_OFFSET_ADJUSTMENT;
			// clamp value
			if (mv < 0) { mv = 0; }
			if (mv >255) { mv = 255; }
			if (pid.getTarget() == 0) { mv = 0; }
			set_output(mv);
				
		}
	}
}

void Heater::set_output(uint8_t value)
{
	element.setHeatingElement(value);
}

void Heater::fail()
{
	fail_state = true;
	set_output(0);
	Motherboard::getBoard().heaterFail(fail_mode);
}

bool Heater::has_failed()
{
	return fail_state;
}
