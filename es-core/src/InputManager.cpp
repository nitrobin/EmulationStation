#include "InputManager.h"

#include "utils/FileSystemUtil.h"
#include "utils/StringUtil.h"
#include "CECInput.h"
#include "Log.h"
#include "platform.h"
#include "Scripting.h"
#include "Window.h"
#include <pugixml.hpp>
#include <SDL.h>
#include <iostream>
#include <assert.h>
#include <cctype>
#include <cstdlib>

#define KEYBOARD_GUID_STRING "-1"
#define CEC_GUID_STRING      "-2"

// SO HEY POTENTIAL POOR SAP WHO IS TRYING TO MAKE SENSE OF ALL THIS (by which I mean my future self)
// There are like four distinct IDs used for joysticks (crazy, right?)
// 1. Device index - this is the "lowest level" identifier, and is just the Nth joystick plugged in to the system (like /dev/js#).
//    It can change even if the device is the same, and is only used to open joysticks (required to receive SDL events).
// 2. SDL_JoystickID - this is an ID for each joystick that is supposed to remain consistent between plugging and unplugging.
//    ES doesn't care if it does, though.
// 3. "Device ID" - this is something I made up and is what InputConfig's getDeviceID() returns.
//    This is actually just an SDL_JoystickID (also called instance ID), but -1 means "keyboard" instead of "error."
// 4. Joystick GUID - this is some squashed version of joystick vendor, version, and a bunch of other device-specific things.
//    It should remain the same across runs of the program/system restarts/device reordering and is what I use to identify which joystick to load.

// hack for cec support
int SDL_USER_CECBUTTONDOWN = -1;
int SDL_USER_CECBUTTONUP   = -1;

InputManager* InputManager::mInstance = NULL;

InputManager::InputManager() : mKeyboardInputConfig(NULL)
{
}

InputManager::~InputManager()
{
	deinit();
}

InputManager* InputManager::getInstance()
{
	if(!mInstance)
		mInstance = new InputManager();

	return mInstance;
}

void InputManager::init()
{
	if(initialized())
		deinit();

	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,
		Settings::getInstance()->getBool("BackgroundJoystickInput") ? "1" : "0");
	// Don't enable the HIDAPI drivers by default, it will break the existing configurations
	// for a few controller types, since the names and the input mappings are different.
#if !defined(_WIN32)
#if	SDL_VERSION_ATLEAST(2,0,9)
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
#endif
#endif
	SDL_InitSubSystem(SDL_INIT_JOYSTICK);
	SDL_JoystickEventState(SDL_ENABLE);

	// first, open all currently present joysticks
	int numJoysticks = SDL_NumJoysticks();
	for(int i = 0; i < numJoysticks; i++)
	{
		addJoystickByDeviceIndex(i);
	}

	mKeyboardInputConfig = new InputConfig(DEVICE_KEYBOARD, "Keyboard", KEYBOARD_GUID_STRING);
	// without a saved keyboard configuration the keyboard works with the defaults
	// (loadDefaultKBConfig: arrows, Enter, Escape, F1, F2, [ ]) instead of not at all
	if(!loadInputConfig(mKeyboardInputConfig))
		loadDefaultKBConfig();

	SDL_USER_CECBUTTONDOWN = SDL_RegisterEvents(2);
	SDL_USER_CECBUTTONUP   = SDL_USER_CECBUTTONDOWN + 1;
	CECInput::init();
	mCECInputConfig = new InputConfig(DEVICE_CEC, "CEC", CEC_GUID_STRING);
	loadInputConfig(mCECInputConfig);
}

void InputManager::addJoystickByDeviceIndex(int id)
{
	assert(id > -1);
	assert(id < SDL_NumJoysticks());

	// open joystick & add to our list
	SDL_Joystick* joy = SDL_JoystickOpen(id);
	assert(joy);

	// add it to our list so we can close it again later
	SDL_JoystickID joyId = SDL_JoystickInstanceID(joy);
	mJoysticks[joyId] = joy;

	char guid[65];
	SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy), guid, 65);

	// create the InputConfig
	mInputConfigs[joyId] = new InputConfig(joyId, SDL_JoystickName(joy), guid);

	// add Vendor and Product IDs
	mInputConfigs[joyId]->setVendorId(SDL_JoystickGetVendor(joy));
	mInputConfigs[joyId]->setProductId(SDL_JoystickGetProduct(joy));

	if(!loadInputConfig(mInputConfigs[joyId]))
	{
		if(loadGameControllerMapping(id, mInputConfigs[joyId]))
			LOG(LogInfo) << "Added joystick '" << SDL_JoystickName(joy) << "' configured from its SDL game controller mapping (GUID: " << guid << ", instance ID: " << joyId << ", device index: " << id << ").";
		else
			LOG(LogInfo) << "Added unconfigured joystick '" << SDL_JoystickName(joy) << "' (GUID: " << guid << ", instance ID: " << joyId << ", device index: " << id << ").";
	}else{
		LOG(LogInfo) << "Added known joystick '" << SDL_JoystickName(joy) << "' (instance ID: " << joyId << ", device index: " << id << ")";
	}

	// set up the prevAxisValues
	int numAxes = SDL_JoystickNumAxes(joy);
	mPrevAxisValues[joyId] = new int[numAxes];
	std::fill(mPrevAxisValues[joyId], mPrevAxisValues[joyId] + numAxes, 0); //initialize array to 0
}

void InputManager::removeJoystickByJoystickID(SDL_JoystickID joyId)
{
	assert(joyId != -1);

	// delete old prevAxisValues
	auto axisIt = mPrevAxisValues.find(joyId);
	delete[] axisIt->second;
	mPrevAxisValues.erase(axisIt);

	// delete old InputConfig
	auto it = mInputConfigs.find(joyId);
	delete it->second;
	mInputConfigs.erase(it);

	// close the joystick
	auto joyIt = mJoysticks.find(joyId);
	LOG(LogInfo) << "Removed joystick '" << SDL_JoystickName(joyIt->second) << "' (instance ID: " << joyId << ")";
	SDL_JoystickClose(joyIt->second);
	mJoysticks.erase(joyIt);
}

void InputManager::deinit()
{
	if(!initialized())
		return;

	for(auto iter = mJoysticks.cbegin(); iter != mJoysticks.cend(); iter++)
	{
		SDL_JoystickClose(iter->second);
	}
	mJoysticks.clear();

	for(auto iter = mInputConfigs.cbegin(); iter != mInputConfigs.cend(); iter++)
	{
		delete iter->second;
	}
	mInputConfigs.clear();

	for(auto iter = mPrevAxisValues.cbegin(); iter != mPrevAxisValues.cend(); iter++)
	{
		delete[] iter->second;
	}
	mPrevAxisValues.clear();

	if(mKeyboardInputConfig != NULL)
	{
		delete mKeyboardInputConfig;
		mKeyboardInputConfig = NULL;
	}

	if(mCECInputConfig != NULL)
	{
		delete mCECInputConfig;
		mCECInputConfig = NULL;
	}

	CECInput::deinit();

	SDL_JoystickEventState(SDL_DISABLE);
	SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
}

int InputManager::getNumJoysticks() { return (int)mJoysticks.size(); }

int InputManager::getAxisCountByDevice(SDL_JoystickID id)
{
	return SDL_JoystickNumAxes(mJoysticks[id]);
}

int InputManager::getButtonCountByDevice(SDL_JoystickID id)
{
	if(id == DEVICE_KEYBOARD)
		return 120; //it's a lot, okay.
	else if(id == DEVICE_CEC)
#ifdef HAVE_CECLIB
		return CEC::CEC_USER_CONTROL_CODE_MAX;
#else // HAVE_LIBCEF
		return 0;
#endif // HAVE_CECLIB
	else
		return SDL_JoystickNumButtons(mJoysticks[id]);
}

InputConfig* InputManager::getInputConfigByDevice(int device)
{
	if(device == DEVICE_KEYBOARD)
		return mKeyboardInputConfig;
	else if(device == DEVICE_CEC)
		return mCECInputConfig;
	else
		return mInputConfigs[device];
}

bool InputManager::parseEvent(const SDL_Event& ev, Window* window)
{
	bool causedEvent = false;
	switch(ev.type)
	{
	case SDL_JOYAXISMOTION:
		//if it switched boundaries
		if((abs(ev.jaxis.value) > DEADZONE) != (abs(mPrevAxisValues[ev.jaxis.which][ev.jaxis.axis]) > DEADZONE))
		{
			int normValue;
			if(abs(ev.jaxis.value) <= DEADZONE)
				normValue = 0;
			else
				if(ev.jaxis.value > 0)
					normValue = 1;
				else
					normValue = -1;

			window->input(getInputConfigByDevice(ev.jaxis.which), Input(ev.jaxis.which, TYPE_AXIS, ev.jaxis.axis, normValue, false));
			causedEvent = true;
		}

		mPrevAxisValues[ev.jaxis.which][ev.jaxis.axis] = ev.jaxis.value;
		return causedEvent;

	case SDL_JOYBUTTONDOWN:
	case SDL_JOYBUTTONUP:
		window->input(getInputConfigByDevice(ev.jbutton.which), Input(ev.jbutton.which, TYPE_BUTTON, ev.jbutton.button, ev.jbutton.state == SDL_PRESSED, false));
		return true;

	case SDL_JOYHATMOTION:
		window->input(getInputConfigByDevice(ev.jhat.which), Input(ev.jhat.which, TYPE_HAT, ev.jhat.hat, ev.jhat.value, false));
		return true;

	case SDL_KEYDOWN:
		if(ev.key.keysym.sym == SDLK_BACKSPACE && SDL_IsTextInputActive())
		{
			window->textInput("\b");
		}

		if(ev.key.repeat)
			return false;

		if(ev.key.keysym.sym == SDLK_F4)
		{
			SDL_Event* quit = new SDL_Event();
			quit->type = SDL_QUIT;
			SDL_PushEvent(quit);
			return false;
		}

		window->input(getInputConfigByDevice(DEVICE_KEYBOARD), Input(DEVICE_KEYBOARD, TYPE_KEY, ev.key.keysym.sym, 1, false));
		return true;

	case SDL_KEYUP:
		window->input(getInputConfigByDevice(DEVICE_KEYBOARD), Input(DEVICE_KEYBOARD, TYPE_KEY, ev.key.keysym.sym, 0, false));
		return true;

	case SDL_TEXTINPUT:
		window->textInput(ev.text.text);
		break;

	case SDL_JOYDEVICEADDED:
		addJoystickByDeviceIndex(ev.jdevice.which); // ev.jdevice.which is a device index
		return true;

	case SDL_JOYDEVICEREMOVED:
		removeJoystickByJoystickID(ev.jdevice.which); // ev.jdevice.which is an SDL_JoystickID (instance ID)
		return false;
	}

	if((ev.type == (unsigned int)SDL_USER_CECBUTTONDOWN) || (ev.type == (unsigned int)SDL_USER_CECBUTTONUP))
	{
		window->input(getInputConfigByDevice(DEVICE_CEC), Input(DEVICE_CEC, TYPE_CEC_BUTTON, ev.user.code, ev.type == (unsigned int)SDL_USER_CECBUTTONDOWN, false));
		return true;
	}

	return false;
}

bool InputManager::loadInputConfig(InputConfig* config)
{
	std::string path = getConfigPath();
	if(!Utils::FileSystem::exists(path))
		return false;

	pugi::xml_document doc;
	pugi::xml_parse_result res = doc.load_file(path.c_str());

	if(!res)
	{
		LOG(LogError) << "Error parsing input config: " << res.description();
		return false;
	}

	pugi::xml_node root = doc.child("inputList");
	if(!root)
		return false;

	pugi::xml_node configNode = root.find_child_by_attribute("inputConfig", "deviceGUID", config->getDeviceGUIDString().c_str());
	if(!configNode)
		configNode = root.find_child_by_attribute("inputConfig", "deviceName", config->getDeviceName().c_str());
	if(!configNode)
		return false;

	config->loadFromXML(configNode);
	return true;
}

// A joystick without a saved configuration that SDL knows as a game controller (SDL's mapping
// database, or the mapping SDL makes itself for a Linux evdev gamepad) gets its inputs from that
// mapping, so it works without the configuration screen. Positions as on Xbox / PlayStation pads:
// accept ("a") is the bottom face button (SDL "a"), back ("b") the right one (SDL "b"), "x" the top
// one (SDL "y"), "y" the left one (SDL "x"). Kept in memory only: configuring the device in the menu
// writes es_input.cfg, which wins from then on. False (config left empty) without a mapping or
// without an accept button in it.
bool InputManager::loadGameControllerMapping(int deviceIndex, InputConfig* config)
{
	char* mapping = SDL_GameControllerMappingForDeviceIndex(deviceIndex);
	if(mapping == NULL)
		return false;
	const std::string text = mapping;
	SDL_free(mapping);

	// SDL element -> ES input: a button, a hat direction or a half axis
	static const std::map<std::string, std::string> buttons = {
		{ "a", "a" }, { "b", "b" }, { "y", "x" }, { "x", "y" }, { "start", "start" }, { "back", "select" },
		{ "leftshoulder", "leftshoulder" }, { "rightshoulder", "rightshoulder" },
		{ "lefttrigger", "lefttrigger" }, { "righttrigger", "righttrigger" },
		{ "leftstick", "leftthumb" }, { "rightstick", "rightthumb" },
		{ "dpup", "up" }, { "dpdown", "down" }, { "dpleft", "left" }, { "dpright", "right" } };
	// SDL stick axis -> ES inputs of its negative and its positive direction
	static const std::map<std::string, std::pair<std::string, std::string>> sticks = {
		{ "leftx", { "leftanalogleft", "leftanalogright" } }, { "lefty", { "leftanalogup", "leftanalogdown" } },
		{ "rightx", { "rightanalogleft", "rightanalogright" } }, { "righty", { "rightanalogup", "rightanalogdown" } } };

	const int device = config->getDeviceId();
	config->clear();
	// "GUID,name,element:source,...": skip the GUID and the name
	const std::vector<std::string> fields = Utils::String::delimitedStringToVector(text, ",");
	for(size_t i = 2; i < fields.size(); i++)
	{
		const size_t colon = fields[i].find(':');
		if(colon == std::string::npos || colon == 0)
			continue;
		const std::string element = fields[i].substr(0, colon);
		std::string source = fields[i].substr(colon + 1);
		int half = 0; // source "+a2" / "-a2": that half of the axis
		if(!source.empty() && (source[0] == '+' || source[0] == '-'))
		{
			half = source[0] == '+' ? 1 : -1;
			source = source.substr(1);
		}
		const bool inverted = !source.empty() && source.back() == '~';
		if(inverted)
			source.pop_back();
		if(source.size() < 2 || !isdigit((unsigned char)source[1]))
			continue;
		const int index = atoi(source.c_str() + 1);

		const auto button = buttons.find(element);
		const auto stick = sticks.find(element);
		if(button != buttons.cend())
		{
			if(source[0] == 'b')
				config->mapInput(button->second, Input(device, TYPE_BUTTON, index, 1, true));
			else if(source[0] == 'h' && source.find('.') != std::string::npos)
				config->mapInput(button->second, Input(device, TYPE_HAT, index, atoi(source.c_str() + source.find('.') + 1), true));
			else if(source[0] == 'a') // a trigger, or a d-pad on an axis: pressed = the positive half unless told
				config->mapInput(button->second, Input(device, TYPE_AXIS, index, (half ? half : 1) * (inverted ? -1 : 1), true));
		}
		else if(stick != sticks.cend() && source[0] == 'a' && half == 0)
		{
			config->mapInput(stick->second.first, Input(device, TYPE_AXIS, index, inverted ? 1 : -1, true));
			config->mapInput(stick->second.second, Input(device, TYPE_AXIS, index, inverted ? -1 : 1, true));
		}
	}

	Input accept;
	if(!config->getInputByName("a", &accept))
	{
		config->clear();
		return false;
	}
	return true;
}

//used in an "emergency" where no keyboard config could be loaded from the inputmanager config file
//allows the user to select to reconfigure in menus if this happens without having to delete es_input.cfg manually
void InputManager::loadDefaultKBConfig()
{
	InputConfig* cfg = getInputConfigByDevice(DEVICE_KEYBOARD);

	cfg->clear();
	cfg->mapInput("up", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_UP, 1, true));
	cfg->mapInput("down", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_DOWN, 1, true));
	cfg->mapInput("left", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_LEFT, 1, true));
	cfg->mapInput("right", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_RIGHT, 1, true));

	cfg->mapInput("a", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_RETURN, 1, true));
	cfg->mapInput("b", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_ESCAPE, 1, true));
	cfg->mapInput("start", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_F1, 1, true));
	cfg->mapInput("select", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_F2, 1, true));

	cfg->mapInput("leftshoulder", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_LEFTBRACKET, 1, true));
	cfg->mapInput("rightshoulder", Input(DEVICE_KEYBOARD, TYPE_KEY, SDLK_RIGHTBRACKET, 1, true));
}

void InputManager::writeDeviceConfig(InputConfig* config)
{
	assert(initialized());

	std::string path = getConfigPath();

	pugi::xml_document doc;

	if(Utils::FileSystem::exists(path))
	{
		// merge files
		pugi::xml_parse_result result = doc.load_file(path.c_str());
		if(!result)
		{
			LOG(LogError) << "Error parsing input config: " << result.description();
		}
		else
		{
			// successfully loaded, delete the old entry if it exists
			pugi::xml_node root = doc.child("inputList");
			if(root)
			{
				// if inputAction @type=onfinish is set, let onfinish command take care for creating input configuration.
				// we just put the input configuration into a temporary input config file.
				pugi::xml_node actionnode = root.find_child_by_attribute("inputAction", "type", "onfinish");
				if(actionnode)
				{
					path = getTemporaryConfigPath();
					doc.reset();
					root = doc.append_child("inputList");
				}
				else
				{
					pugi::xml_node oldEntry = root.find_child_by_attribute("inputConfig", "deviceGUID",
											  config->getDeviceGUIDString().c_str());
					if(oldEntry)
					{
						root.remove_child(oldEntry);
					}
					oldEntry = root.find_child_by_attribute("inputConfig", "deviceName",
															config->getDeviceName().c_str());
					if(oldEntry)
					{
						root.remove_child(oldEntry);
					}
				}
			}
		}
	}

	pugi::xml_node root = doc.child("inputList");
	if(!root)
		root = doc.append_child("inputList");

	config->writeToXML(root);
	doc.save_file(path.c_str());

	Scripting::fireEvent("config-changed");
	Scripting::fireEvent("controls-changed");

	// execute any onFinish commands and re-load the config for changes
	doOnFinish();
	loadInputConfig(config);
}

void InputManager::doOnFinish()
{
	assert(initialized());
	std::string path = getConfigPath();
	pugi::xml_document doc;

	if(Utils::FileSystem::exists(path))
	{
		pugi::xml_parse_result result = doc.load_file(path.c_str());
		if(!result)
		{
			LOG(LogError) << "Error parsing input config: " << result.description();
		}
		else
		{
			pugi::xml_node root = doc.child("inputList");
			if(root)
			{
				root = root.find_child_by_attribute("inputAction", "type", "onfinish");
				if(root)
				{
					for(pugi::xml_node command = root.child("command"); command;
							command = command.next_sibling("command"))
					{
						std::string tocall = command.text().get();

						LOG(LogInfo) << "	" << tocall;
						std::cout << "==============================================\ninput config finish command:\n";
						int exitCode = runSystemCommand(tocall);
						std::cout << "==============================================\n";

						if(exitCode != 0)
						{
							LOG(LogWarning) << "...launch terminated with nonzero exit code " << exitCode << "!";
						}
					}
				}
			}
		}
	}
}

std::string InputManager::getConfigPath()
{
	std::string path = Utils::FileSystem::getHomePath();
	path += "/.emulationstation/es_input.cfg";
	return path;
}

std::string InputManager::getTemporaryConfigPath()
{
	std::string path = Utils::FileSystem::getHomePath();
	path += "/.emulationstation/es_temporaryinput.cfg";
	return path;
}

bool InputManager::initialized() const
{
	return mKeyboardInputConfig != NULL;
}

int InputManager::getNumConfiguredDevices()
{
	int num = 0;
	for(auto it = mInputConfigs.cbegin(); it != mInputConfigs.cend(); it++)
	{
		if(it->second->isConfigured())
			num++;
	}

	if(mKeyboardInputConfig->isConfigured())
		num++;

	if(mCECInputConfig->isConfigured())
		num++;

	return num;
}

bool InputManager::hasUnconfiguredJoystick()
{
	for(auto it = mInputConfigs.cbegin(); it != mInputConfigs.cend(); it++)
	{
		if(!it->second->isConfigured())
			return true;
	}
	return false;
}

std::string InputManager::getDeviceGUIDString(int deviceId)
{
	if(deviceId == DEVICE_KEYBOARD)
		return KEYBOARD_GUID_STRING;

	if(deviceId == DEVICE_CEC)
		return CEC_GUID_STRING;

	auto it = mJoysticks.find(deviceId);
	if(it == mJoysticks.cend())
	{
		LOG(LogError) << "getDeviceGUIDString - deviceId " << deviceId << " not found!";
		return "something went horribly wrong";
	}

	char guid[65];
	SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(it->second), guid, 65);
	return std::string(guid);
}
