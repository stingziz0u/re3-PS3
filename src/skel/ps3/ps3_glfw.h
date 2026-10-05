// ps3_glfw.h -- the few GLFW names the game's input code uses (Pad.cpp,
// ControllerConfig.cpp, Frontend.cpp), implemented on the PS3 pad in
// ps3_pad.cpp. No window, no mouse, no keyboard: those calls return nothing.
#pragma once

typedef struct GLFWwindow GLFWwindow;

#define GLFW_PRESS 1
#define GLFW_RELEASE 0

#define GLFW_KEY_D 68
#define GLFW_KEY_R 82

#define GLFW_MOUSE_BUTTON_1 0
#define GLFW_MOUSE_BUTTON_2 1
#define GLFW_MOUSE_BUTTON_3 2
#define GLFW_MOUSE_BUTTON_4 3
#define GLFW_MOUSE_BUTTON_5 4
#define GLFW_MOUSE_BUTTON_LEFT GLFW_MOUSE_BUTTON_1
#define GLFW_MOUSE_BUTTON_RIGHT GLFW_MOUSE_BUTTON_2
#define GLFW_MOUSE_BUTTON_MIDDLE GLFW_MOUSE_BUTTON_3

#define GLFW_CURSOR 0x00033001
#define GLFW_CURSOR_NORMAL 0x00034001
#define GLFW_CURSOR_HIDDEN 0x00034002
#define GLFW_CURSOR_DISABLED 0x00034003

#define GLFW_JOYSTICK_1 0
#define GLFW_JOYSTICK_LAST 15

// gamepad buttons, GLFW 3.3 numbering (A = cross ... like an Xbox layout)
#define GLFW_GAMEPAD_BUTTON_A 0
#define GLFW_GAMEPAD_BUTTON_B 1
#define GLFW_GAMEPAD_BUTTON_X 2
#define GLFW_GAMEPAD_BUTTON_Y 3
#define GLFW_GAMEPAD_BUTTON_LEFT_BUMPER 4
#define GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER 5
#define GLFW_GAMEPAD_BUTTON_BACK 6
#define GLFW_GAMEPAD_BUTTON_START 7
#define GLFW_GAMEPAD_BUTTON_GUIDE 8
#define GLFW_GAMEPAD_BUTTON_LEFT_THUMB 9
#define GLFW_GAMEPAD_BUTTON_RIGHT_THUMB 10
#define GLFW_GAMEPAD_BUTTON_DPAD_UP 11
#define GLFW_GAMEPAD_BUTTON_DPAD_RIGHT 12
#define GLFW_GAMEPAD_BUTTON_DPAD_DOWN 13
#define GLFW_GAMEPAD_BUTTON_DPAD_LEFT 14
#define GLFW_GAMEPAD_BUTTON_LAST GLFW_GAMEPAD_BUTTON_DPAD_LEFT

#define GLFW_GAMEPAD_AXIS_LEFT_X 0
#define GLFW_GAMEPAD_AXIS_LEFT_Y 1
#define GLFW_GAMEPAD_AXIS_RIGHT_X 2
#define GLFW_GAMEPAD_AXIS_RIGHT_Y 3
#define GLFW_GAMEPAD_AXIS_LEFT_TRIGGER 4
#define GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER 5
#define GLFW_GAMEPAD_AXIS_LAST GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER

void glfwGetCursorPos(GLFWwindow *window, double *xpos, double *ypos);
void glfwSetCursorPos(GLFWwindow *window, double xpos, double ypos);
int glfwGetMouseButton(GLFWwindow *window, int button);
int glfwGetKey(GLFWwindow *window, int key);
void glfwSetInputMode(GLFWwindow *window, int mode, int value);
int glfwJoystickPresent(int jid);
const char *glfwGetJoystickName(int jid);
const unsigned char *glfwGetJoystickButtons(int jid, int *count);

#define GLFW_CONNECTED 0x00040001
#define GLFW_DISCONNECTED 0x00040002

typedef struct GLFWgamepadstate
{
	unsigned char buttons[15];
	float axes[6];
} GLFWgamepadstate;

const float *glfwGetJoystickAxes(int jid, int *count);
int glfwGetGamepadState(int jid, GLFWgamepadstate *state);
int glfwJoystickIsGamepad(int jid);

// ps3_pad.cpp: reads the DualShock 3 (once per frame, from CapturePad)
void PS3_PadInit(void);
void PS3_PadPoll(void);
void PS3_PadShutdown(void);
