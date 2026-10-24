/*
 * s8swipe.c, part of libs8gl.dylib (tools/dreamlte/ca/build.sh): boot-arg s8swipe=T1[,T2...] makes
 * a finger swipe at those seconds after boot, through a touch screen service of ours in
 * backboardd's HID event server, so that tools/dreamlte/s8shot.c can take other screens than the
 * lock screen with nobody at the phone. A time alone is a swipe up from the bottom edge (on a phone
 * with no passcode it unlocks it; in an application it goes home); with a letter after it:
 *   l   right to left across the middle (the next home screen page, the App Library)
 *   r   left to right across the middle
 *   d   down from the top edge at the left (Notification Center)
 *   c   down from the top edge at the right (Control Center)
 *   tXXYY  a tap at XX % of the width, YY % of the height (t6136: Settings on the home screen)
 * e.g. s8swipe=100,150l,220d. s8swipe=@ reads the list (up to 64, separated by commas, spaces or
 * lines) from the drop zone file "swipes" (/private/var/tmp/s8drop/swipes), for longer runs than
 * the boot-args have room for.
 * libs8gl is loaded only into backboardd (libs8ca.dylib, boot-arg s8ca=1).
 *
 * The touch screen service is HackiOS's userland/src/inject.c (MIT, see LICENSE.hackios), cut down.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

/* (stderr goes to the kernel log: libs8ca.dylib) */
#define say(...) fprintf(stderr, __VA_ARGS__)

/* IOKit's HID event system, the part with no public header on iOS (as HackiOS's inject.c) */
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;

struct virtual_service_callbacks {      /* IOHIDVirtualServiceClientCallbacksV2 */
	uint64_t version;               /* 2 */
	void (*notification)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, CFDictionaryRef property);
	Boolean (*setProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value);
	CFTypeRef (*copyProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key);
	IOHIDEventRef (*copyEvent)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, IOHIDEventRef matching, uint32_t options);
	int (*setOutputEvent)(void *target, void *refcon, IOHIDServiceClientRef service, IOHIDEventRef event);
	CFTypeRef (*copyMatchingEvent)(void *target, void *refcon, IOHIDServiceClientRef service, CFDictionaryRef matching);
};

extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef allocator, int type, CFDictionaryRef attributes);
extern void IOHIDEventSystemClientScheduleWithDispatchQueue(IOHIDEventSystemClientRef client, dispatch_queue_t queue);
extern IOHIDServiceClientRef IOHIDVirtualServiceClientCreateWithCallbacks(IOHIDEventSystemClientRef client, CFDictionaryRef attributes,
    const struct virtual_service_callbacks *callbacks, void *target, void *refcon);
extern Boolean IOHIDVirtualServiceClientRemove(IOHIDServiceClientRef service);
extern Boolean IOHIDVirtualServiceClientDispatchEvent(IOHIDServiceClientRef service, IOHIDEventRef event);
extern IOHIDEventRef IOHIDEventCreateDigitizerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t transducer, uint32_t index,
    uint32_t identity, uint32_t event_mask, uint32_t button_mask, double x, double y, double z, double tip_pressure,
    double barrel_pressure, double twist, Boolean range, Boolean touch, uint32_t options);
extern IOHIDEventRef IOHIDEventCreateDigitizerFingerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t index, uint32_t identity,
    uint32_t event_mask, double x, double y, double z, double tip_pressure, double twist, Boolean range, Boolean touch,
    uint32_t options);
extern void IOHIDEventAppendEvent(IOHIDEventRef parent, IOHIDEventRef child, uint32_t options);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef event, uint32_t field, CFIndex value);

#define HID_CLIENT_SIMPLE               4
#define HID_CLIENT_RATE_CONTROLLED      3
#define HID_PAGE_DIGITIZER              0x0d
#define HID_USAGE_TOUCH_SCREEN          0x04
#define DIGITIZER_HAND                  3
#define DIGITIZER_RANGE                 0x1
#define DIGITIZER_TOUCH                 0x2
#define DIGITIZER_POSITION              0x4
#define DIGITIZER_FROM_EDGE_TIP         0x800   /* the finger came in over an edge (UIKit's edge gestures want it) */
#define FIELD_IS_BUILT_IN               0x4
#define FIELD_DIGITIZER_DISPLAY_INTEGRATED 0xb0019
#define SWIPES                          64

static long swipe_at[SWIPES];
static char swipe_kind[SWIPES];
static double swipe_x[SWIPES], swipe_y[SWIPES];   /* taps */
static int swipe_count;
static volatile bool service_opened;

static CFTypeRef
service_copy_property(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key)
{
	CFTypeRef value = CFDictionaryGetValue((CFDictionaryRef)refcon, key);

	(void)target;
	(void)service;
	return value ? CFRetain(value) : NULL;
}

static Boolean
service_set_property(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value)
{
	(void)target;
	(void)refcon;
	(void)service;
	/* the last thing the event server does when it opens a service */
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) &&
	    CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value)) {
		service_opened = true;
	}
	return true;
}

static void
service_notification(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, CFDictionaryRef property)
{
	(void)target;
	(void)refcon;
	(void)service;
	(void)type;
	(void)property;
}

static const struct virtual_service_callbacks service_callbacks = {
	.version = 2, .notification = service_notification, .setProperty = service_set_property,
	.copyProperty = service_copy_property,
};

static void
set_number(CFMutableDictionaryRef dictionary, CFStringRef key, int value)
{
	CFNumberRef number = CFNumberCreate(NULL, kCFNumberIntType, &value);

	CFDictionarySetValue(dictionary, key, number);
	CFRelease(number);
}

/*
 * A built-in touch screen inside the event server, made again until the server has opened it
 * (backboardd, whose server this is, may still be starting: see HackiOS's make_service()).
 */
static IOHIDServiceClientRef
make_touch_screen(IOHIDEventSystemClientRef client)
{
	CFMutableDictionaryRef properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	IOHIDServiceClientRef service = NULL;
	CFArrayRef pairs;

	set_number(pair, CFSTR("DeviceUsagePage"), HID_PAGE_DIGITIZER);
	set_number(pair, CFSTR("DeviceUsage"), HID_USAGE_TOUCH_SCREEN);
	pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
	set_number(properties, CFSTR("PrimaryUsagePage"), HID_PAGE_DIGITIZER);
	set_number(properties, CFSTR("PrimaryUsage"), HID_USAGE_TOUCH_SCREEN);
	set_number(properties, CFSTR("DeviceUsagePage"), HID_PAGE_DIGITIZER);
	set_number(properties, CFSTR("DeviceUsage"), HID_USAGE_TOUCH_SCREEN);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(properties, CFSTR("Built-In"), kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("DisplayIntegrated"), kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("Transport"), CFSTR("SPI"));
	CFDictionarySetValue(properties, CFSTR("Product"), CFSTR("s8ca swipe"));
	set_number(properties, CFSTR("VendorID"), 0);
	set_number(properties, CFSTR("ProductID"), 0);
	for (int attempt = 1; attempt <= 20; attempt++) {
		service_opened = false;
		service = IOHIDVirtualServiceClientCreateWithCallbacks(client, properties, &service_callbacks, NULL, properties);
		if (service == NULL) {
			break;
		}
		for (int tenths = 0; tenths < 20 && !service_opened; tenths++) {
			usleep(100000);
		}
		if (service_opened) {
			break;
		}
		IOHIDVirtualServiceClientRemove(service);
		CFRelease(service);
		service = NULL;
	}
	return service;
}

/* One frame of the touch screen: the hand, and the one finger under it. */
static void
send_finger(IOHIDServiceClientRef screen, uint32_t change, bool down, double x, double y, bool edge)
{
	uint64_t now = mach_absolute_time();
	IOHIDEventRef hand, finger;

	hand = IOHIDEventCreateDigitizerEvent(NULL, now, DIGITIZER_HAND, 0, 0, change & ~DIGITIZER_RANGE, 0, x, y, 0, 0, 0, 0,
	    down, down, 0);
	IOHIDEventSetIntegerValue(hand, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
	IOHIDEventSetIntegerValue(hand, FIELD_IS_BUILT_IN, 1);
	finger = IOHIDEventCreateDigitizerFingerEvent(NULL, now, 1, 2, change | (edge ? DIGITIZER_FROM_EDGE_TIP : 0), x, y, 0,
	    down ? 1.0 : 0.0, 0, down, down, 0);
	IOHIDEventSetIntegerValue(finger, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
	IOHIDEventAppendEvent(hand, finger, 0);
	CFRelease(finger);
	IOHIDVirtualServiceClientDispatchEvent(screen, hand);
	CFRelease(hand);
}

static long
seconds_since_boot(void)
{
	struct timeval boot, now;
	size_t size = sizeof(boot);

	sysctlbyname("kern.boottime", &boot, &size, NULL, 0);
	gettimeofday(&now, NULL);
	return now.tv_sec - boot.tv_sec;
}

static void *
swipe_thread(void *arg)
{
	static const int types[] = { HID_CLIENT_SIMPLE, HID_CLIENT_RATE_CONTROLLED };
	IOHIDServiceClientRef screen = NULL;

	(void)arg;
	sleep(3);                       /* let backboardd start its event server */
	for (unsigned i = 0; screen == NULL && i < sizeof(types) / sizeof(types[0]); i++) {
		IOHIDEventSystemClientRef client = IOHIDEventSystemClientCreateWithType(NULL, types[i], NULL);

		if (client != NULL) {
			IOHIDEventSystemClientScheduleWithDispatchQueue(client, dispatch_queue_create("s8ca.swipe", NULL));
			screen = make_touch_screen(client);
		}
	}
	say("s8ca: swipe touch screen service %p\n", (void *)screen);
	for (int k = 0; screen != NULL && k < swipe_count; k++) {
		while (seconds_since_boot() < swipe_at[k]) {
			sleep(1);
		}
		/* from (x0, y0) to (x1, y1) in a quarter of a second; up: from the home bar to the middle */
		char kind = swipe_kind[k];
		double x0 = 0.5, y0 = 0.995, x1 = 0.5, y1 = 0.495;
		bool edge = true;

		if (kind == 't') {
			send_finger(screen, DIGITIZER_RANGE | DIGITIZER_TOUCH, true, swipe_x[k], swipe_y[k], false);
			usleep(80000);
			send_finger(screen, DIGITIZER_RANGE | DIGITIZER_TOUCH, false, swipe_x[k], swipe_y[k], false);
			say("s8ca: tapped %.2f,%.2f at %ld s\n", swipe_x[k], swipe_y[k], seconds_since_boot());
			continue;
		}
		if (kind == 'l' || kind == 'r') {
			x0 = kind == 'l' ? 0.85 : 0.15;
			x1 = kind == 'l' ? 0.15 : 0.85;
			y0 = y1 = 0.5;
			edge = false;
		} else if (kind == 'd' || kind == 'c') {
			x0 = x1 = kind == 'd' ? 0.25 : 0.85;
			y0 = 0.002;
			y1 = 0.6;
		}
		send_finger(screen, DIGITIZER_RANGE | DIGITIZER_TOUCH, true, x0, y0, edge);
		for (int step = 1; step <= 16; step++) {
			usleep(16000);
			send_finger(screen, DIGITIZER_POSITION, true, x0 + (x1 - x0) * step / 16, y0 + (y1 - y0) * step / 16, edge);
		}
		usleep(16000);
		send_finger(screen, DIGITIZER_RANGE | DIGITIZER_TOUCH, false, x1, y1, edge);
		say("s8ca: swiped %c at %ld s\n", kind != 0 ? kind : 'u', seconds_since_boot());
	}
	return NULL;
}

static void
swipe_later(const char *arguments)
{
	const char *at = strstr(arguments, "s8swipe=");
	pthread_t thread;

	static char list[2048];

	if (at == NULL || (at != arguments && at[-1] != ' ')) {
		return;
	}
	at += 8;
	if (*at == '@') {
		FILE *f = fopen("/private/var/tmp/s8drop/swipes", "r");
		size_t n = f != NULL ? fread(list, 1, sizeof(list) - 1, f) : 0;

		if (f != NULL) {
			fclose(f);
		}
		list[n] = 0;
		at = list;
		say("s8ca: swipes from the drop zone: %zu bytes\n", n);
	}
	for (;;) {
		while (*at == ',' || *at == ' ' || *at == '\n' || *at == '\r') {
			at++;
		}
		if (swipe_count >= SWIPES || *at < '0' || *at > '9') {
			break;
		}
		char *end;

		swipe_at[swipe_count] = strtol(at, &end, 10);
		swipe_kind[swipe_count] = *end >= 'a' && *end <= 'z' ? *end++ : 0;
		if (swipe_kind[swipe_count] == 't') {
			long xy = strtol(end, &end, 10);
			swipe_x[swipe_count] = (xy / 100) / 100.0;
			swipe_y[swipe_count] = (xy % 100) / 100.0;
		}
		swipe_count++;
		at = end;
	}
	if (swipe_count > 0 && pthread_create(&thread, NULL, swipe_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}

__attribute__((constructor))
static void
s8swipe_init(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *name = getprogname();

	if (name != NULL && strcmp(name, "backboardd") == 0 &&
	    sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		swipe_later(arguments);
	}
}
