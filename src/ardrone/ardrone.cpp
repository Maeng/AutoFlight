#include "../afconstants.h"
#include "ardrone.h"

#include "atcommands/configcommand.h"
#include "atcommands/configidscommand.h"
#include "atcommands/controlcommand.h"
#include "atcommands/attitudecommand.h"
#include "atcommands/emergencycommand.h"
#include "atcommands/flattrimcommand.h"
#include "atcommands/flipcommand.h"
#include "atcommands/hovercommand.h"
#include "atcommands/landcommand.h"
#include "atcommands/magnetometercalibrationcommand.h"
#include "atcommands/recordonusbcommand.h"
#include "atcommands/resetwatchdogcommand.h"
#include "atcommands/takeoffcommand.h"
#include "atcommands/zapcommand.h"

#include <Gamepad.h>

#include <chrono>
#include <thread>
#include <cmath>

#ifndef M_PI
	#define M_PI 3.141592653589793238462643383279502884197169399375105820974944592307816406
#endif

using namespace std;

ARDrone::ARDrone()
{
	_ip = ardrone::DEFAULT_IP;
}

ARDrone::ARDrone(string ip)
{
	_ip = ip;
}

ARDrone::~ARDrone()
{
	if(!_stop_flag)
	{
		stopUpdateLoop();
	}
}

void ARDrone::setIP(string ip)
{
	_ip = ip;
}

int ARDrone::connect()
{
	if(!_connected)
	{
		try
		{
			// Initialize communication with AR.Drone
			_cl.init(_ip, _io_service);
		
			// Needed for the AR.Drone to send full navigation data and accept commands
			_cl.setAppID();
			boost::this_thread::sleep_for(boost::chrono::milliseconds(250)); // Wait until the drone has performed its configuration switch (important)
			_cl.sendATCommands(vector<ATCommand>{ConfigIDSCommand(), ConfigCommand("general:navdata_demo", "FALSE"), ConfigIDSCommand(), ConfigCommand("general:navdata_options", "268435455"), ControlCommand(5)});
		
			// Wait for the AR.Drone to process the commands
			boost::this_thread::sleep_for(boost::chrono::milliseconds(50));
		
			drone_setConfiguration(ardrone::config::VIDEO_CODEC, ardrone::config::codec::MP4_360P);

			// Init navdata manager
			_nm.init(_ip, _io_service);

			// Wait for navdata packets to be received
			boost::this_thread::sleep_for(boost::chrono::milliseconds(100));

			// Process received packets (if any)
			_nm.update();

			// Check that navdata has indeed been received
			_connected = _nm.isConnected();

			if(_connected)
			{
				_vm.init(_ip, _io_service);
				_vm.update();

				return ardrone::connection::CONNECTION_ESTABLISHED;
			}
			else
			{
				_cl.close();
				_nm.close();
				return ardrone::connection::CONNECTION_FAILED;
			}
		}
		catch(const boost::system::system_error &e)
		{
			cout << "Error: " << e.what() << endl;
			if(e.code().value() != 1)
			{
				try
				{
					_cl.close();
				}
				catch(const boost::system::system_error &e)
				{}
			}
			return ardrone::connection::EXCEPTION_OCCURRED;
		}
	}
	else
	{
		return ardrone::connection::ALREADY_CONNECTED;
	}
}

bool ARDrone::isConnected()
{
	return _connected;
}

void ARDrone::addNavdataListener(INavdataListener *listener)
{
	_ndlisteners.push_back(listener);
}

void ARDrone::removeNavdataListener(INavdataListener *listener)
{
	_ndlisteners.erase(remove(_ndlisteners.begin(), _ndlisteners.end(), listener), _ndlisteners.end());
}

void ARDrone::addVideoListener(IVideoListener *listener)
{
	_vlisteners.push_back(listener);
}

void ARDrone::removeVideoListener(IVideoListener *listener)
{
	_vlisteners.erase(remove(_vlisteners.begin(), _vlisteners.end(), listener), _vlisteners.end());
}

void ARDrone::setControllerConfiguration(ControllerConfiguration *config)
{
	if(config != NULL)
	{
		delete _controllerconfig;
	}

	_controllerconfig = config;
}

void ARDrone::startUpdateLoop()
{
	if(_updater == NULL)
	{
		if(_connected)
		{
			_stop_flag = false;
			_updater = new boost::thread(&ARDrone::runUpdateLoop, this);
		}
		else
		{
			throw NotConnectedException();
		}
	}
}

void ARDrone::stopUpdateLoop()
{
	_stop_flag = true;
	_connected = false;

	if(_updater != NULL)
	{
		try
		{
			_updater->join();
		}
		catch(boost::thread_interrupted &)
		{}

		delete _updater;
		_updater = NULL;
	}
}

void ARDrone::runUpdateLoop()
{
	cout << "Update loop started" << endl;

	while(!_stop_flag)
	{
		boost::this_thread::sleep_for(boost::chrono::milliseconds(25));

		// Process received packets (if any): Important!
		_io_service.poll();

		// Get newest navdata
		_navdatamutex.lock();
		_nd = _nm.getNavdata();
		if(_nd != NULL)
		{
			for(INavdataListener *i : _ndlisteners)
			{
				i->navdataAvailable(_nd);
			}
		}
		_navdatamutex.unlock();

		processControllerInput();

		// Process command queue
		_commandmutex.lock();
		if((_phi == 0.0f) && (_theta == 0.0f) && (_gaz == 0.0f) && (_yaw == 0.0f))
		{
			_commandqueue.push_back(HoverCommand());
		}
		else
		{
			_commandqueue.push_back(AttitudeCommand(_phi, _theta, _gaz, _yaw));
		}
		_cl.sendATCommands(_commandqueue);
		_commandqueue.clear();
		_commandmutex.unlock();

		// Get newest video frame
		_frame = _vm.getVideoFrame();
		if(!_frame.empty())
		{
			for(IVideoListener *i : _vlisteners)
			{
				i->videoFrameAvailable(_frame);
			}
		}
	}

	cout << "Update loop stopped" << endl;
}

void ARDrone::processControllerInput()
{
	if(_controllerconfig != NULL)
	{
		Gamepad_device *device = Gamepad_deviceAtIndex(_controllerconfig->deviceID);
		bool slow = false;

		if(_controllerconfig->takeoff >= 0)
		{
			if(device->buttonStates[_controllerconfig->takeoff])
			{
				drone_takeOff();
			}
		}
		if(_controllerconfig->land >= 0)
		{
			if(device->buttonStates[_controllerconfig->land])
			{
				drone_land();
			}
		}
		if(_controllerconfig->zap >= 0)
		{
			if(device->buttonStates[_controllerconfig->zap])
			{
				drone_toggleView();
			}
		}
		if(_controllerconfig->photo >= 0)
		{
			if(device->buttonStates[_controllerconfig->photo])
			{
				drone_takePicture();
			}
		}
		if(_controllerconfig->takeoff >= 0)
		{
			if(device->buttonStates[_controllerconfig->recording])
			{
				drone_toggleRecording();
			}
		}
		if(_controllerconfig->takeoff >= 0)
		{
			if(device->buttonStates[_controllerconfig->flip])
			{
				drone_flip(ardrone::flip::LEFT);
			}
		}
		if(_controllerconfig->takeoff >= 0)
		{
			if(device->buttonStates[_controllerconfig->slow])
			{
				slow = true;
			}
		}

		float phi = 0, theta = 0, gaz = 0, yaw = 0;

		if(_controllerconfig->rollM >= 0 && _controllerconfig->rollP >= 0)
		{
			phi += device->buttonStates[_controllerconfig->rollP];
			phi -= device->buttonStates[_controllerconfig->rollM];
		}
		else if(_controllerconfig->roll >= 0)
		{
			phi = device->axisStates[_controllerconfig->roll];
		}

		if(_controllerconfig->pitchM >= 0 && _controllerconfig->pitchP >= 0)
		{
			theta += device->buttonStates[_controllerconfig->pitchP];
			theta -= device->buttonStates[_controllerconfig->pitchM];
		}
		else if(_controllerconfig->roll >= 0)
		{
			theta = device->axisStates[_controllerconfig->pitch];
		}

		if(_controllerconfig->heightM >= 0 && _controllerconfig->heightP >= 0)
		{
			gaz += device->buttonStates[_controllerconfig->heightP];
			gaz -= device->buttonStates[_controllerconfig->heightM];
		}
		else if(_controllerconfig->roll >= 0)
		{
			gaz = device->axisStates[_controllerconfig->height];
		}

		if(_controllerconfig->yawM >= 0 && _controllerconfig->yawP >= 0)
		{
			yaw += device->buttonStates[_controllerconfig->yawP];
			yaw -= device->buttonStates[_controllerconfig->yawM];
		}
		else if(_controllerconfig->roll >= 0)
		{
			yaw = device->axisStates[_controllerconfig->yaw];
		}

		if(slow)
		{
			phi *= 0.3f;
			theta *= 0.3f;
			gaz *= 0.5f;
			yaw *= 0.5f;
		}

		if(abs(phi) < 0.1f || abs(theta) < 0.1f || abs(gaz) < 0.1f || abs(yaw) < 0.1f)
		{
			// Ignore very small values
			drone_hover();
		}
		else
		{
			drone_move(phi, theta, gaz, yaw);
		}
	}
}

// AR.Drone navdata getting functions

ardrone::orientation ARDrone::drone_getOrientation()
{
	if(!isConnected())
	{
		ardrone::orientation error;
		error.yaw = -1;
		error.pitch = -1;
		error.roll = -1;
		return error;
	}

	ardrone::orientation o;

	_navdatamutex.lock();
	o.yaw = _nd->psi;
	o.pitch = _nd->theta;
	o.roll = _nd->phi;
	_navdatamutex.unlock();

	return o;
}

ardrone::linearvelocity ARDrone::drone_getLinearVelocity()
{
	if(!isConnected())
	{
		ardrone::linearvelocity error;
		error.vx = -1;
		error.vy = -1;
		error.vz = -1;
		return error;
	}

	ardrone::linearvelocity l;

	_navdatamutex.lock();
	l.vx = _nd->vx;
	l.vy = _nd->vy;
	l.vz = _nd->vz;
	_navdatamutex.unlock();

	return l;
}

float ARDrone::drone_getAltitude()
{
	if(!isConnected())
	{
		return -1;
	}

	float a;

	_navdatamutex.lock();
	a = _nd->altitude;
	_navdatamutex.unlock();

	return a;
}

ardrone::position ARDrone::drone_getPosition() //TODO: Implement this!
{
	if(!isConnected())
	{
		ardrone::position error;
		error.x = -1;
		error.y = -1;
		return error;
	}

	ardrone::position p;

	_navdatamutex.lock();

	_navdatamutex.unlock();

	return p;
}

ardrone::acceleration ARDrone::drone_getAcceleration()
{
	if(!isConnected())
	{
		ardrone::acceleration error;
		error.ax = -1;
		error.ay = -1;
		error.az = -1;
		return error;
	}

	ardrone::acceleration a;

	_navdatamutex.lock();
	a.ax = _nd->ax;
	a.ay = _nd->ay;
	a.az = _nd->az;
	_navdatamutex.unlock();

	return a;
}

float ARDrone::drone_getBatteryStatus()
{
	if(!isConnected())
	{
		return -1;
	}

	float b;

	_navdatamutex.lock();
	b = _nd->vbatpercentage;
	_navdatamutex.unlock();

	return b;
}

bool ARDrone::drone_isFlying()
{
	if(!isConnected())
	{
		return false;
	}

	return _flying;
}

bool ARDrone::drone_isRecording()
{
	if(!isConnected())
	{
		return false;
	}

	return _isRecording;
}

// AR.Drone control functions

bool ARDrone::drone_calibmagneto()
{
	if(!isConnected())
	{
		return false;
	}

	_commandmutex.lock();
	_commandqueue.push_back(MagnetometerCalibrationCommand());
	_commandmutex.unlock();

	return true;
}

bool ARDrone::drone_changeView(int view)
{
	if(!isConnected())
	{
		return false;
	}

	_commandmutex.lock();
	_commandqueue.push_back(ConfigIDSCommand());
	_commandqueue.push_back(ZapCommand(view == ardrone::camera::FRONT? true : false));
	_commandmutex.unlock();

	_currentView = view;

	return true;
}

bool ARDrone::drone_emergency()
{
	if(!isConnected())
	{
		return false;
	}

	_commandmutex.lock();
	_commandqueue.push_back(EmergencyCommand(true));
	_commandmutex.unlock();

	_flying = false;

	return true;
}

bool ARDrone::drone_flattrim()
{
	if(!isConnected())
	{
		return false;
	}

	_commandmutex.lock();
	_commandqueue.push_back(FlatTrimCommand());
	_commandmutex.unlock();

	return true;
}

bool ARDrone::drone_flip(int direction)
{
	if(!isConnected())
	{
		return false;
	}

	_commandmutex.lock();
	_commandqueue.push_back(FlipCommand(direction));
	_commandmutex.unlock();

	return true;
}

bool ARDrone::drone_hover()
{
	if(!isConnected())
	{
		return false;
	}

	_phi = 0.0f;
	_theta = 0.0f;
	_gaz = 0.0f;
	_yaw = 0.0f;

	return true;
}

bool ARDrone::drone_land()
{
	if(!isConnected())
	{
		return false;
	}

	_commandmutex.lock();
	_commandqueue.push_back(LandCommand());
	_commandmutex.unlock();

	_flying = false;

	return true;
}

bool ARDrone::drone_move(float phi, float theta, float gaz, float yaw)
{
	if(!isConnected())
	{
		return false;
	}

	_phi = phi;
	_theta = theta;
	_gaz = gaz;
	_yaw = yaw;

	return true;
}

bool ARDrone::drone_setPhi(float phi)
{
	if(!isConnected())
	{
		return false;
	}

	_phi = phi;

	return true;
}

bool ARDrone::drone_setTheta(float theta)
{
	if(!isConnected())
	{
		return false;
	}

	_theta = theta;

	return true;
}

bool ARDrone::drone_setGaz(float gaz)
{
	if(!isConnected())
	{
		return false;
	}

	_gaz = gaz;

	return true;
}

bool ARDrone::drone_setYaw(float yaw)
{
	if(!isConnected())
	{
		return false;
	}

	_yaw = yaw;

	return true;
}

bool ARDrone::drone_pair(string mac_address)
{
	if(!isConnected())
	{
		return false;
	}

	_commandmutex.lock();
	_commandqueue.push_back(ConfigIDSCommand());
	_commandqueue.push_back(ConfigCommand("network:owner_mac", mac_address));
	_commandmutex.unlock();

	return true;
}

bool ARDrone::drone_pair()
{
	if(!isConnected())
	{
		return false;
	}

	//TODO: get MAC address of wifi adapter

	return true;
}

bool ARDrone::drone_setConfiguration(string field, float value)
{
	if(!isConnected())
	{
		return false;
	}

	if((field == ardrone::config::YAW_SPEED_MAX) || (field == ardrone::config::TILT_MAX))
	{
		value = value * M_PI / 180.0; // Convert degree to radian
	}
	else
	{
		if(field == ardrone::config::ALTITUDE_MAX)
		{
			value = value * 10;
		}
	}

	_commandmutex.lock();
	_commandqueue.push_back(ConfigIDSCommand());
	_commandqueue.push_back(ConfigCommand(field, to_string(value)));
	_commandmutex.unlock();

	return true;
}

bool ARDrone::drone_setConfiguration(string field, string value)
{
	if(!isConnected())
	{
		return false;
	}

	_commandmutex.lock();
	_commandqueue.push_back(ConfigIDSCommand());
	_commandqueue.push_back(ConfigCommand(field, value));
	_commandmutex.unlock();

	return true;
}

bool ARDrone::drone_startRecordingOnUSB()
{
	if(!isConnected())
	{
		return false;
	}

	_commandmutex.lock();
	_commandqueue.push_back(ConfigIDSCommand());
	_commandqueue.push_back(RecordOnUSBCommand(true));
	_commandmutex.unlock();

	return true;
}

bool ARDrone::drone_stopRecordingOnUSB()
{
	if(!isConnected())
	{
		return false;
	}

	_commandmutex.lock();
	_commandqueue.push_back(ConfigIDSCommand());
	_commandqueue.push_back(RecordOnUSBCommand(false));
	_commandmutex.unlock();

	return true;
}

bool ARDrone::drone_takeOff()
{
	if(!isConnected())
	{
		return false;
	}

	_commandmutex.lock();
	_commandqueue.push_back(TakeOffCommand());
	_commandmutex.unlock();

	_flying = true;

	return true;
}

bool ARDrone::drone_toggleView()
{
	if(!isConnected())
	{
		return false;
	}

	if(_currentView == ardrone::camera::BOTTOM)
	{
		drone_changeView(ardrone::camera::FRONT);
	}
	else
	{
		drone_changeView(ardrone::camera::BOTTOM);
	}

	return true;
}

bool ARDrone::drone_unpair()
{
	if(!isConnected())
	{
		return false;
	}

	drone_pair("00:00:00:00:00:00");

	return true;
}

bool ARDrone::drone_takePicture()
{
	if(!isConnected())
	{
		return false;
	}

	const boost::posix_time::ptime now= boost::posix_time::second_clock::local_time();
	string timestamp = to_iso_timestamp(now);

	string filename = "Pic_";
	filename.append(timestamp);
	filename.append(".jpg");

	_vm.takePicture(filename);

	return true;
}

bool ARDrone::drone_startRecording()
{
	if(!isConnected())
	{
		return false;
	}

	// Tell the drone to start sending the record stream
	drone_setConfiguration(ardrone::config::VIDEO_CODEC, ardrone::config::codec::MP4_360P_H264_720P);

	const boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
	string timestamp = to_iso_timestamp(now);

	string filename = "Vid_";
	filename.append(timestamp);
	filename.append(".mp4");

	try
	{
		if(_vm.startRecording(filename))
		{
			_isRecording = true;
		}
	}
	catch(runtime_error &e)
	{
		cout << "Error: " << e.what() << endl;
	}

	return true;
}

bool ARDrone::drone_stopRecording()
{
	if(!isConnected())
	{
		return false;
	}

	try
	{
		if(_vm.stopRecording())
		{
			_isRecording = false;
		}
	}
	catch(runtime_error &e)
	{
		cout << "Error: " << e.what() << endl;
	}

	// Tell the drone to stop sending the record stream
	drone_setConfiguration(ardrone::config::VIDEO_CODEC, ardrone::config::codec::MP4_360P);

	return true;
}

bool ARDrone::drone_toggleRecording()
{
	if(!isConnected())
	{
		return false;
	}

	if(_isRecording)
	{
		drone_stopRecording();
	}
	else
	{
		drone_startRecording();
	}

	return true;
}

string ARDrone::to_iso_timestamp(boost::posix_time::ptime time)
{
	stringstream timestamp;
	timestamp << setw(4) << setfill('0') << time.date().year() << setw(2) << time.date().month() << setw(2) << time.date().day();
	timestamp << "T";
	timestamp << setw(2) << time.time_of_day().hours() << setw(2) << time.time_of_day().minutes() << setw(2) << time.time_of_day().seconds();

	return timestamp.str();
}