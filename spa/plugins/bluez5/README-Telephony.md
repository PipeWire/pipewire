# PipeWire Bluetooth Telephony service

The Telephony service is a D-Bus service that allows applications to communicate
with the HFP native backend in order to control phone calls. Phone call features
are a core part of the HFP specification and are available when a mobile phone
is paired (therefore, PipeWire acts as the Hands-Free and the phone is the Audio
Gateway).

The service is exposed on the user session bus by default, but there is an
option to make it available on the system bus instead.

The service implements its own interfaces alongside the standard DBus
Introspectable, ObjectManager and Properties interfaces, where needed.
These interfaces are mostly compatible with the ofono Manager, VoiceCallManager
and VoiceCall interfaces. For compatibility, the `org.ofono.Manager`,
`org.ofono.VoiceCallManager` and `org.ofono.VoiceCall` are also implemented
with any additional compatibility methods & signals are necessary to allow
ofono-based applications to be able to work just by modifying the service name,
the manager object path and the operating bus (session vs system).

In addition, to the compatibility interfaces, there is a runtime option to
also register the service as `org.ofono` on the system bus, making it a drop-in
replacement for ofono. Note, however, that this service is not a full replacement,
but only for the Bluetooth-based voice calls.

## Manager Object

```
Service         org.freedesktop.PipeWire.Telephony
            or  org.ofono
Object path     /org/freedesktop/PipeWire/Telephony
            or  /
Implements      org.ofono.Manager
                org.freedesktop.DBus.Introspectable
                org.freedesktop.DBus.ObjectManager
```

The manager object is always available and allows applications to get access to
the connected audio gateways.

The object path is set to `/` when ofono service compatibility is enabled,
in which case the service name `org.ofono` is also registered instead of
`org.freedesktop.PipeWire.Telephony`.

The methods and signals below are made available on the `org.ofono.Manager`
interface, for compatibility. AudioGateway objects are normally announced via
the standard DBus ObjectManager interface.

### Methods

`array{object,dict} GetModems()`

Get an array of AudioGateway objects and properties that represents the
currently connected audio gateways.

### Signals

`ModemAdded(object path, dict properties)`

Signal that is sent when a new audio gateway is added. It contains the object
path of the new audio gateway and also its properties.

`ModemRemoved(object path)`

Signal that is sent when an audio gateway has been removed. The object path is
no longer accessible after this signal and only emitted for reference.

## AudioGateway Object

```
Service         org.freedesktop.PipeWire.Telephony
            or  org.ofono
Object path     /org/freedesktop/PipeWire/Telephony/{ag0,ag1,...}
Implements      org.freedesktop.PipeWire.Telephony.AudioGateway1
                org.ofono.VoiceCallManager
                org.freedesktop.DBus.Introspectable
                org.freedesktop.DBus.ObjectManager
```

Audio gateway objects represent the currently connected AG devices (typically
mobile phones).

The methods, signals and properties listed below are made available on both
`org.freedesktop.PipeWire.Telephony.AudioGateway1` and
`org.ofono.VoiceCallManager` interfaces, unless explicitly documented otherwise.

Call objects are announced via both the standard DBus ObjectManager interface
and via the `org.ofono.VoiceCallManager` interface, for compatibility.

### Methods

`array{object,dict} GetCalls()`

Get an array of call object paths and properties that represents the currently
present calls.

This method call should only be used once when an application starts up.
Further call additions and removal shall be monitored via CallAdded and
CallRemoved signals.

NOTE: This method is implemented only on the `org.ofono.VoiceCallManager`
interface, for compatibility. Call announcements are normally made available via
the standard `org.freedesktop.DBus.ObjectManager` interface.

`object Dial(string number)`

Initiates a new outgoing call. Returns the object path to the newly created
call.

The number must be a string containing the following characters:
`[0-9+*#,ABCD]{1,80}` In other words, it must be a non-empty string consisting
of 1 to 80 characters. The character set can contain numbers, `+`, `*`, `#`, `,`
and the letters `A` to `D`. Besides this sanity checking no further number
validation is performed. It is assumed that the gateway and/or the network will
perform further validation.

NOTE: If an active call (single or multiparty) exists, then it is automatically
put on hold if the dial procedure is successful.

Possible Errors:
 * org.freedesktop.PipeWire.Telephony.Error.InvalidState
 * org.freedesktop.DBus.Error.InvalidArgs
 * org.freedesktop.DBus.Error.Failed

`void SwapCalls()`

Swaps Active and Held calls. The effect of this is that all calls (0 or more
including calls in a multi-party conversation) that were Active are now Held,
and all calls (0 or more) that were Held are now Active.

GSM specification does not allow calls to be swapped in the case where Held,
Active and Waiting calls exist. Some modems implement this anyway, thus it is
manufacturer specific whether this method will succeed in the case of Held,
Active and Waiting calls.

Possible Errors:
 * org.freedesktop.PipeWire.Telephony.Error.InvalidState
 * org.freedesktop.DBus.Error.Failed

`void ReleaseAndAnswer()`

Releases currently active call (0 or more) and answers the currently waiting
call. Please note that if the current call is a multiparty call, then all
parties in the multi-party call will be released.

Possible Errors:
 * org.freedesktop.PipeWire.Telephony.Error.InvalidState
 * org.freedesktop.DBus.Error.Failed

`void ReleaseAndSwap()`

Releases currently active call (0 or more) and activates any currently held
calls. Please note that if the current call is a multiparty call, then all
parties in the multi-party call will be released.

Possible Errors:
 * org.freedesktop.PipeWire.Telephony.Error.InvalidState
 * org.freedesktop.DBus.Error.Failed

`void HoldAndAnswer()`

Puts the current call (including multi-party calls) on hold and answers the
currently waiting call. Calling this function when a user already has a both
Active and Held calls is invalid, since in GSM a user can have only a single
Held call at a time.

Possible Errors:
 * org.freedesktop.PipeWire.Telephony.Error.InvalidState
 * org.freedesktop.DBus.Error.Failed

`void HangupAll()`

Releases all calls except waiting calls. This includes multiparty calls.

Possible Errors:
 * org.freedesktop.PipeWire.Telephony.Error.InvalidState
 * org.freedesktop.DBus.Error.Failed

`array{object} CreateMultiparty()`

Joins active and held calls together into a multi-party call. If one of the
calls is already a multi-party call, then the other call is added to the
multiparty conversation. Returns the new list of calls participating in the
multiparty call.

There can only be one subscriber controlled multi-party call according to the
GSM specification.

Possible Errors:
 * org.freedesktop.PipeWire.Telephony.Error.InvalidState
 * org.freedesktop.DBus.Error.Failed

`void SendTones(string tones)`

Sends the DTMF tones to the network. The tones have a fixed duration. Tones
can be one of: '0' - '9', '*', '#', 'A', 'B', 'C', 'D'. The last four are
typically not used in normal circumstances.

Possible Errors:
 * org.freedesktop.PipeWire.Telephony.Error.InvalidState
 * org.freedesktop.DBus.Error.InvalidArgs
 * org.freedesktop.DBus.Error.Failed

### Signals

`CallAdded(object path, dict properties)`

Signal that is sent when a new call is added.  It contains the object path of
the new voice call and also its properties.

NOTE: This method is implemented only on the `org.ofono.VoiceCallManager`
interface, for compatibility. Call announcements are normally made available via
the standard `org.freedesktop.DBus.ObjectManager` interface.

`CallRemoved(object path)`

Signal that is sent when a voice call has been released. The object path is no
longer accessible after this signal and only emitted for reference.

NOTE: This method is implemented only on the `org.ofono.VoiceCallManager`
interface, for compatibility. Call announcements are normally made available via
the standard `org.freedesktop.DBus.ObjectManager` interface.

## Call Object

```
Service         org.freedesktop.PipeWire.Telephony
            or  org.ofono
Object path     /org/freedesktop/PipeWire/Telephony/{ag0,ag1,...}/{call0,call1,...}
Implements      org.freedesktop.PipeWire.Telephony.Call1
                org.ofono.VoiceCall
                org.freedesktop.DBus.Introspectable
                org.freedesktop.DBus.Properties
```

Call objects represent active calls and allow managing them.

The methods, signals and properties listed below are made available on both
`org.freedesktop.PipeWire.Telephony.Call1` and `org.ofono.VoiceCall`
interfaces, unless explicitly documented otherwise.

### Methods

`dict GetProperties()`

Returns all properties for this object. See the properties section for available
properties.

NOTE: This method is implemented only on the `org.ofono.VoiceCall` interface,
for compatibility. Properties are normally made available via the standard
`org.freedesktop.DBus.Properties` interface.

`void Answer()`

Answers an incoming call. Only valid if the state of the call is "incoming".

Possible Errors:
 * org.freedesktop.PipeWire.Telephony.Error.InvalidState
 * org.freedesktop.DBus.Error.Failed

`void Hangup()`

Hangs up the call.

For an incoming call, the call is hung up using ATH or equivalent. For a
waiting call, the remote party is notified by using the User Determined User
Busy (UDUB) condition. This is generally implemented using CHLD=0.

Please note that the GSM specification does not allow the release of a held call
when a waiting call exists. This is because 27.007 allows CHLD=1X to operate
only on active calls. Hence a held call cannot be hung up without affecting the
state of the incoming call (e.g. using other CHLD alternatives). Most
manufacturers provide vendor extensions that do allow the state of the held call
to be modified using CHLD=1X or equivalent. It should be noted that Bluetooth
HFP specifies the classic 27.007 behavior and does not allow CHLD=1X to modify
the state of held calls.

Based on the discussion above, it should also be noted that releasing a
particular party of a held multiparty call might not be possible on some
implementations. It is recommended for the applications to structure their UI
accordingly.

NOTE: Releasing active calls does not produce side-effects. That is the state
of held or waiting calls is not affected. As an exception, in the case where a
single active call and a waiting call are present, releasing the active call
will result in the waiting call transitioning to the 'incoming' state.

Possible Errors:
 * org.freedesktop.PipeWire.Telephony.Error.InvalidState
 * org.freedesktop.DBus.Error.Failed

### Signals

`PropertyChanged(string property, variant value)`

Signal is emitted whenever a property has changed. The new value is passed as
the signal argument.

NOTE: This method is implemented only on the `org.ofono.VoiceCall` interface,
for compatibility. Properties are normally made available via the standard
`org.freedesktop.DBus.Properties` interface.

### Properties

`string LineIdentification [readonly]`

Contains the Line Identification information returned by the network, if
present. For incoming calls this is effectively the CLIP. For outgoing calls
this attribute will hold the dialed number, or the COLP if received by the
audio gateway.

Please note that COLP may be different from the dialed number. A special
"withheld" value means the remote party refused to provide caller ID and the
"override category" option was not provisioned for the current subscriber.

`string IncomingLine [readonly, optional]`

Contains the Called Line Identification information returned by the network.
This is only available for incoming calls and indicates the local subscriber
number which was dialed by the remote party. This is useful for subscribers
which have a multiple line service with their network provider and would like to
know what line the call is coming in on.

`string Name [readonly]`

Contains the Name Identification information returned by the network, if
present.

`boolean Multiparty [readonly]`

Contains the indication if the call is part of a multiparty call or not.

Notifications if a call becomes part or leaves a multiparty call are sent.

`string State [readonly]`

Contains the state of the current call. The state can be one of:
  - "active" - The call is active
  - "held" - The call is on hold
  - "dialing" - The call is being dialed
  - "alerting" - The remote party is being alerted
  - "incoming" - Incoming call in progress
  - "waiting" - Call is waiting
  - "disconnected" - No further use of this object is allowed, it will be
       destroyed shortly
