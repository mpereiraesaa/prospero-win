# Wired Xbox 360 input

The title and Wine exchange four independent XInput users through pw_wine_sink.
The existing _at API and user-0 wrappers are preserved. Device capability APIs
add subtype, vendor/product/revision and flags; Wine patch0472 reports them.
Paired PE/Unix XInput modules must be built and deployed together.

The portable src/pw_xinput_usb parser accepts exactly the 20-byte wired-360
00/14 input transfer. Button bits, triggers and signed axes pass through unchanged.
It validates cached device/interface/endpoint descriptors: alt 0, FF/5D/01,
interrupt IN, 20..64-byte maximum packet. Known 1430:4748 X-plorer identity uses
XInput subtype GuitarAlternate (7); unlisted compatible wired-360 devices retain gamepad
subtype (1). Identity never depends on a fixed USB bus/address.

native/pw_xinput_usb_freebsd owns /dev/usb enumeration, cached descriptor ioctls,
O_RDONLY|O_NONBLOCK endpoint opens, short-transfer setup and bounded reads.
It has no reset, configuration request, driver detach, output report or USB FS
claim. Descriptor ioctl numbers and FIFO short-transfer setup follow FreeBSD 9
sys/dev/usb/usb_ioctl.h (105/107/108/113). The PS5 ABI and read path require their
own target qualification; FreeBSD source alone is not proof of firmware behavior.

The bridge keeps user0 for the title pad. Other signed-in DualSenses reserve
their existing users before USB input is published. A USB device keeps a free
user1..3 until a DualSense claims it, then moves to a free user or waits with its
latest state cached. Releasing a reservation republishes the cached USB input.
USB disconnect/close never clears a different owner's user. All users occupied
means no USB injection; it never overwrites a DualSense. USB input stays neutral
while the runtime menu, system pad interception or on-screen keyboard owns input.
The backend scans every five seconds, drains at most eight transfers per device
per poll and connects only after a valid report. Empty transfers preserve state;
real errors or missing devfs nodes disconnect.
No new worker thread is started. Guitar output/rumble is not implemented and the
USB users advertise no force-feedback capability. Without the indexed runtime
exports, normal USB injection remains disabled.

PW_USB360_CAPTURE=1 builds a diagnostic raw-input capture: button transitions and significant analog movements are logged as hexadecimal
with independent 256-record budgets and explicit limit markers. Idle analog jitter
cannot spend the button budget. Capture selection never changes the delivered
state. This mode has no USB XInput injection.
This mode can qualify interrupt input against the existing Wine runtime before
rebuilding its PE/Unix XInput modules. It is not a gameplay qualification.

Host checks: test_pw_xinput_usb, test_pw_xinput_usb_freebsd, test_pw_xinput_usb_wine,
test_pw_wine_sink and test_xinput_title_slots.py. The latter executes the patch's
actual Unix dispatch and PE state/capability/rumble helpers against the real sink.
They establish host logic only. Target report interpretation, physical fret/strum/
whammy mapping, hotplug, independent controller enumeration and gameplay remain
separate acceptance boundaries.

The parser/read path and earlier fixed-slot bridge were qualified on PS5 FW5.50
with a wired1430:4748 X-plorer: all five frets, strums, Start/Back, whammy and tilt;
independent Windows XInput users0/1; empty-read preservation; physical hotplug;
and unmodified YARG menu input. The upstream _at/capability adaptation and slot
arbitration here have host tests; they still need target requalification. Three
concurrent USB devices, other models and USB rumble/output are not qualified.
