import QtQuick

// What the guide says. Kept apart from HelpGuide.qml so a website can be made
// from the same words later.
//
// kind "task": steps the coach walks through on the real window. A step's
//   target is the objectName of the control to point at; done(backend, run, ui)
//   moves on by itself, and a step without done waits for Next. run is what
//   the walk-through remembers (run.seen; run.step is cleared at each step);
//   ui.shown(name) says whether a control is on screen, sheets included.
//   tips are shown under the steps in the guide.
// kind "part": a part of the window. points are the basics; more is for
//   someone who already knows them. target is what "Show me where" points at.
// kind "fix": a problem, with what to try in order.
// also: extra words people search with that the text does not use.
QtObject {
    readonly property var topics: [
        // ── Learn by doing ─────────────────────────────────────────────
        { key: "first-recording", kind: "task", icon: "circle", level: qsTr("Beginner"), time: qsTr("About 3 minutes"),
          name: qsTr("Make your first recording"),
          summary: qsTr("Record your screen with your voice and save it as a video file. Nothing goes online."),
          also: "record capture screen video file save start begin new",
          related: ["camera-mic", "settings-recording", "cant-find-recording"],
          finish: qsTr("Your recording is saved. Show my recordings opens the folder it is in."),
          steps: [
              { title: qsTr("Add what you want to record"),
                text: qsTr("Click Display to record a whole screen, or Window for one app. Your desktop then asks which one to share."),
                target: "addDisplayButton",
                done: (b, run) => b.items.length > 0 },
              { title: qsTr("Check the picture"),
                text: qsTr("The big picture in the middle is exactly what will be recorded. Drag a corner to make something smaller, or drag it to move it."),
                target: "studioPreview" },
              { title: qsTr("Check your sound"),
                text: qsTr("Click + in Sources and choose your microphone under Microphone or audio input, or Desktop sound for the computer's sound. Say something: the bars in the Audio mixer should move. Green is good, yellow is loud, red is too loud. If nothing moves, see No sound in the guide."),
                target: "addSourceButton" },
              { title: qsTr("Press Record"),
                text: qsTr("Recording starts at once, and a clock beside the button shows how long it has run. Ctrl+R does the same."),
                target: "recordButton",
                done: (b, run) => { if (b.recording) run.seen = true; return b.recording } },
              { title: qsTr("Stop when you are done"),
                text: qsTr("Press Stop. The file is finished in a moment and goes to the OmaBroadcast folder in your Videos, or the folder chosen in Settings ▸ Recording."),
                target: "recordButton",
                done: (b, run) => run.seen && !b.recording && !b.recordingStopping }
          ],
          tips: [
              qsTr("Not sure which settings suit you? Tools ▸ Set up for… asks what you are making, tests this computer and sets everything for you."),
              qsTr("Recordings are MKV files by default, which survive a crash. Settings ▸ Recording ▸ Also make an MP4 copy when it ends gives you a file that plays everywhere too.")
          ] },

        { key: "go-live", kind: "task", icon: "radio", level: qsTr("Beginner"), time: qsTr("About 5 minutes"),
          name: qsTr("Go live on Twitch or YouTube"),
          summary: qsTr("Connect your channel with its stream key, then start and end a live stream."),
          also: "stream streaming broadcast live twitch youtube facebook restream key rtmp channel",
          related: ["settings-streaming", "stream-fails", "dropped-frames"],
          finish: qsTr("That is a whole stream. Next time, Go live is all it takes: the stream key is remembered."),
          steps: [
              { title: qsTr("Open Settings"),
                text: qsTr("Click the cog at the top right, or press Ctrl+comma."),
                target: "settingsButton",
                done: (b, run, ui) => ui.shown("settingsNav") },
              { title: qsTr("Go to Streaming"),
                text: qsTr("Choose Streaming in the list on the left."),
                target: "settingsPage_streaming",
                done: (b, run, ui) => ui.shown("settingsStreaming") },
              { title: qsTr("Add a stream"),
                text: qsTr("Press the Add a stream… button. You can add one for each place you go live to; all the ones ticked go out together."),
                target: "addStreamButton",
                done: (b, run, ui) => ui.shown("streamService") },
              { title: qsTr("Choose the service and paste your key"),
                text: qsTr("Pick Twitch, YouTube, Facebook Live or Restream, paste the stream key, and press Save. On Twitch the key is in Creator Dashboard ▸ Settings ▸ Stream; on YouTube, in YouTube Studio ▸ Create ▸ Go live."),
                target: "streamService",
                done: (b, run, ui) => !ui.shown("streamService") && b.streamTargets.length > 0 },
              { title: qsTr("Close Settings"),
                text: qsTr("Press Close. Your stream key is kept in the system keyring, not in a plain file."),
                target: "settingsNav",
                done: (b, run, ui) => !ui.shown("settingsNav") },
              { title: qsTr("Go live"),
                text: qsTr("Press Go live. The button turns into End stream and a clock starts beside it."),
                target: "streamButton",
                done: (b, run) => { if (b.streaming) run.seen = true; return b.streaming } },
              { title: qsTr("Check it arrives"),
                text: qsTr("Open your channel or its dashboard: the picture shows after a few seconds. In Stats, Sending behind should stay near zero; if it climbs, your upload is too slow for the bitrate."),
                target: "statsDock" },
              { title: qsTr("End the stream"),
                text: qsTr("Press End stream when you are done."),
                target: "streamButton",
                done: (b, run) => run.seen && !b.streaming && !b.streamingStopping }
          ],
          tips: [
              qsTr("YouTube can take up to a day to switch on live streaming for a new channel."),
              qsTr("Tools ▸ Set up for… ▸ Streaming tests your upload with a short stream that never goes live, and picks a bitrate that fits."),
              qsTr("Never show your stream key on screen: anyone with it can stream to your channel. If it leaks, reset it on the service's dashboard.")
          ] },

        { key: "camera-mic", kind: "task", icon: "camera", level: qsTr("Beginner"), time: qsTr("About 4 minutes"),
          name: qsTr("Add your camera and microphone"),
          summary: qsTr("Put yourself in the picture, and make your voice clear and steady."),
          also: "webcam camera capture card hdmi usb face cam microphone mic voice facecam picture in picture",
          related: ["no-camera", "audio-filters", "no-sound", "green-screen"],
          steps: [
              { title: qsTr("Add your camera"),
                text: qsTr("Click + in Sources, then Capture device, then your camera or capture card."),
                target: "addSourceButton",
                done: (b) => b.items.some(i => i.kind === "device") },
              { title: qsTr("Choose its size and speed"),
                text: qsTr("With the camera selected, its settings are on the right. Best available picks its biggest, smoothest picture; 1920 × 1080 at 30 or 60 frames a second is a good start."),
                target: "inspectorDock" },
              { title: qsTr("Put it where you want it"),
                text: qsTr("Drag a corner of the camera in the picture to make it smaller, then drag it into a corner. Alt and drag an edge to crop it."),
                target: "studioPreview" },
              { title: qsTr("Pick your microphone"),
                text: qsTr("Click + in Sources, then Microphone or audio input, then the one you speak into. A camera's own microphone is in that list too, if you want to use it."),
                target: "addSourceButton" },
              { title: qsTr("Make your voice clearer"),
                text: qsTr("Press the waveform button on your microphone's strip (FX when the meters run up and down). Add RNNoise speech suppression to take out fans and hum, then Compressor to even out loud and quiet words."),
                target: "mixerDock" },
              { title: qsTr("Set the level"),
                text: qsTr("Speak as you will when recording. The loudest words should reach yellow but never red. Move your microphone's slider until they do."),
                target: "mixerDock" }
          ],
          tips: [
              qsTr("A camera works in one app at a time. Close video calls and other camera apps first."),
              qsTr("Headphones stop your speakers being picked up by the microphone, which causes echo.")
          ] },

        { key: "green-screen", kind: "task", icon: "palette", level: qsTr("Intermediate"), time: qsTr("About 4 minutes"),
          name: qsTr("Use a green screen"),
          summary: qsTr("Take out the backdrop behind you, and put yourself over a picture, a game or anything else."),
          also: "chroma key keying green screen blue screen background remove backdrop matte ai person light wrap edges",
          related: ["remove-background", "filters", "camera-mic", "dropped-frames"],
          steps: [
              { title: qsTr("Select your camera"),
                text: qsTr("Click your camera in Sources."),
                target: "sourcesDock",
                done: (b) => b.selectedItems.length > 0 },
              { title: qsTr("Open its filters"),
                text: qsTr("Press Edit filters… in the Filters section on the right, or right-click the camera and choose Filters… from its menu."),
                target: "inspectorDock",
                done: (b, run, ui) => ui.shown("addVideoFilter") },
              { title: qsTr("Add Chroma key"),
                text: qsTr("Press Add filter at the top left and choose Keying ▸ Chroma key. The backdrop's colour is found by itself."),
                target: "addVideoFilter",
                done: (b, run, ui) => ui.shown("findKeyBackdrop") },
              { title: qsTr("Check the colour"),
                text: qsTr("If some of the screen is left, press Pick from preview and click on the screen in the picture."),
                target: "pickKeyBackdrop" },
              { title: qsTr("Look at the matte"),
                text: qsTr("Tick Show matte: white is kept and black is taken out. Grey in the backdrop? Raise Similarity. Holes in you? Lower it. Hair too sharp? Raise Edge softness."),
                target: "videoFiltersShowMatte" },
              { title: qsTr("Put something behind you"),
                text: qsTr("Close the filters. Add an image or a game with + in Sources, then drag it below your camera in the list: higher in the list is in front."),
                target: "sourcesDock" }
          ],
          tips: [
              qsTr("Light the screen evenly, with its own lights, and stand a metre or two in front of it: shadows and creases are the hardest part to key."),
              qsTr("Avoid wearing green, and anything shiny that reflects the screen."),
              qsTr("Set your camera's focus, exposure and white balance by hand if it can, so the backdrop's colour does not drift."),
              qsTr("Add Light wrap below Chroma key in the filters list: the light of whatever is behind you spills a little over your edges, so you look lit by the scene instead of pasted on."),
              qsTr("No green screen? See Remove your background without a green screen: Remove background (AI) finds people in front of any background, though its edges are softer than a good green screen's.")
          ] },

        { key: "remove-background", kind: "task", icon: "scan", level: qsTr("Intermediate"), time: qsTr("About 3 minutes"),
          name: qsTr("Remove your background without a green screen"),
          summary: qsTr("Take away the room behind you with no green screen at all, and put yourself over a picture, a game or anything else."),
          also: "remove background ai person cut out no green screen blur background bokeh virtual background matte segmentation people light wrap video call",
          related: ["green-screen", "filters", "dropped-frames"],
          steps: [
              { title: qsTr("Select your camera"),
                text: qsTr("Click your camera in Sources."),
                target: "sourcesDock",
                done: (b) => b.selectedItems.length > 0 },
              { title: qsTr("Open its filters"),
                text: qsTr("Press Edit filters… in the Filters section on the right, or right-click the camera and choose Filters… from its menu."),
                target: "inspectorDock",
                done: (b, run, ui) => ui.shown("addVideoFilter") },
              { title: qsTr("Add Remove background (AI)"),
                text: qsTr("Press Add filter at the top left and choose Keying ▸ Remove background (AI). In a moment the room behind you turns to a checkerboard: that part is now see-through."),
                target: "addVideoFilter",
                done: (b, run, ui) => ui.shown("videoFilterChoice_quality") },
              { title: qsTr("Look at the matte"),
                text: qsTr("Tick Show matte: white is kept and black is taken away. A grey rim round you? Raise Tighten edges a little at a time."),
                target: "videoFiltersShowMatte" },
              { title: qsTr("Choose the quality"),
                text: qsTr("Fast is right for most computers. Balanced and Best follow your outline more closely and cost the graphics card more. If recordings stutter, go back to Fast, or set Spread over to 3 frames."),
                target: "videoFilterChoice_quality" },
              { title: qsTr("Put something behind you"),
                text: qsTr("Close the filters. Add an image or a game with + in Sources, then drag it below your camera in the list: higher in the list is in front."),
                target: "sourcesDock" }
          ],
          finish: qsTr("Your background is gone. It follows you as you move, and works whatever room you are in."),
          tips: [
              qsTr("Add Light wrap below Remove background (AI) in the filters list: the light of whatever is behind you spills a little over your edges, so you look lit by the scene instead of pasted on."),
              qsTr("Want to keep your room, just out of focus, as on a video call? Set Background to Blur."),
              qsTr("It finds people only, not objects or pets: a mug you hold up may vanish, or show only where your hand is."),
              qsTr("It works best on one or two people seen from the waist up, lit from the front, and in front of a background that is not too busy."),
              qsTr("Edges are softer than a good green screen gives, above all round loose hair. For the cleanest edges, use a green screen and Chroma key instead."),
              qsTr("It runs on the graphics card, spread over two frames by default, so each frame carries half the work and your outline follows a frame later. Spread over 3 frames is lighter still. Watch Stats for dropped frames the first time, above all at 4K."),
              qsTr("Without a graphics card, if the card stops answering during a show, or if the filter cannot run for any reason such as no memory left, the source is hidden rather than shown with its background; the status line and its Filters window say so.")
          ] },

        { key: "grade", kind: "task", icon: "palette", level: qsTr("Intermediate"), time: qsTr("About 5 minutes"),
          name: qsTr("Grade a camera with colour wheels and scopes"),
          summary: qsTr("Warm the shadows, cool the highlights, set black and white where they belong, watching the scopes instead of guessing by eye."),
          also: "grade grading colour color wheels lift gamma gain offset scopes waveform parade vectorscope histogram skin tone white balance correct",
          related: ["filters", "green-screen"],
          steps: [
              { title: qsTr("Select your camera"),
                text: qsTr("Click your camera in Sources."),
                target: "sourcesDock",
                done: (b) => b.selectedItems.length > 0 },
              { title: qsTr("Open its filters"),
                text: qsTr("Press Edit filters… in the Filters section on the right, or right-click the camera and choose Filters… from its menu."),
                target: "inspectorDock",
                done: (b, run, ui) => ui.shown("addVideoFilter") },
              { title: qsTr("Add Colour wheels"),
                text: qsTr("Press Add filter and choose Colour ▸ Colour wheels. The scopes open beside the preview."),
                target: "addVideoFilter",
                done: (b, run, ui) => ui.shown("colourWheel_lift") },
              { title: qsTr("Set black and white"),
                text: qsTr("Choose Waveform. Move Lift's slider until the darkest part sits just above 0, and Gain's until the brightest sits just below 100."),
                target: "videoFiltersScopeMode" },
              { title: qsTr("Take out a colour cast"),
                text: qsTr("Choose RGB parade. On something white or grey the three should be level: drag the Gain wheel's point away from the colour that is too high (Lift's for the shadows). Hold Shift to move it finely."),
                target: "colourWheel_gain" },
              { title: qsTr("Check skin"),
                text: qsTr("Choose Vectorscope. Faces of every complexion fall near the dashed line; if yours sit off it, nudge Gamma's point back towards it."),
                target: "colourWheel_gamma" }
          ],
          finish: qsTr("Your camera is graded. The grade belongs to the camera, so it looks the same in every studio."),
          tips: [
              qsTr("Each wheel's point pushes its part of the picture towards a colour; the colours sit where the vectorscope shows them, so pushing towards red moves the trace towards R."),
              qsTr("Lift is the shadows, Gamma the mid-tones, Gain the highlights, Offset all of them at once. Double-click a wheel to put its point back in the middle; the arrow beside its name resets the lot."),
              qsTr("Contrast turns about Pivot: what is darker than it gets darker, what is brighter brighter."),
              qsTr("The eye beside the filter turns the grade off and on, to compare."),
              qsTr("Scopes work for any filter: tick Scopes under the preview.")
          ] },

        { key: "phone-camera", kind: "task", icon: "smartphone", level: qsTr("Intermediate"), time: qsTr("About 5 minutes"),
          name: qsTr("Use your phone as a camera"),
          summary: qsTr("Send your phone's camera and microphone to this computer, over Wi-Fi or a USB cable, as one more source."),
          also: "phone iphone android mobile camera microphone wireless srt rtmp moblin larix stream app tether usb hotspot network passphrase password key",
          related: ["sources", "camera-mic", "faq-phone"],
          steps: [
              { title: qsTr("Add a phone source"),
                text: qsTr("Press + in Sources and choose Phone (network stream). This computer starts listening for the phone."),
                target: "addSourceButton",
                done: (b, run, ui) => ui.shown("networkProtocol") },
              { title: qsTr("Choose SRT"),
                text: qsTr("Leave the protocol on SRT: it copes best with Wi-Fi. Choose RTMP only if your phone app offers nothing else."),
                target: "networkProtocol" },
              { title: qsTr("Note the address"),
                text: qsTr("Under the protocol is the address to send to, such as srt://192.168.0.20:9000. Copy puts it on the clipboard."),
                target: "networkSendTo" },
              { title: qsTr("Note the passphrase"),
                text: qsTr("The source made a passphrase, and only a phone that has it gets in. Type it into the app's passphrase field; for an app without one, the source shows an address with the passphrase in it."),
                target: "networkPassphrase" },
              { title: qsTr("Start streaming from the phone"),
                text: qsTr("On an iPhone, the free app Moblin works well (Larix Broadcaster too, with a subscription for some features). Add a stream with that address and passphrase, choose 1080p at 30 or 60, and start it. The picture and sound appear in a second or two."),
                target: "studioPreview" }
          ],
          finish: qsTr("Your phone is a camera. Its sound has a channel of its own in the mixer, already in step with the picture."),
          tips: [
              qsTr("For the steadiest picture, use a cable: turn on Personal Hotspot and plug the phone in. The computer sees it as a wired network; send to the address the source shows for it (often 172.20.10.something). The phone charges as it streams."),
              qsTr("Over a cable, lower Latency to about 50 ms for less delay. Over weak Wi-Fi, raise it."),
              qsTr("If the source says the firewall will block the phone, run the command it shows once, in a terminal."),
              qsTr("A phone needs the passphrase to send here. On SRT what it sends is encrypted too. RTMP puts the passphrase in the address and encrypts nothing, so someone watching the network can read it: prefer SRT."),
              qsTr("If an app cannot do any of this, clear the passphrase. Then anyone who can reach the port can send a picture and sound, on air if the source is showing, so keep that to networks you trust."),
              qsTr("The phone sends H.264 or HEVC, which the graphics card decodes, so even 4K costs the processor almost nothing."),
              qsTr("An iPhone cannot be a plain USB webcam on Linux, so a streaming app is the way. Any app that sends SRT or RTMP works, on Android too.")
          ] },

        { key: "presentation", kind: "task", icon: "presentation", level: qsTr("Beginner"), time: qsTr("About 2 minutes"),
          name: qsTr("Show a presentation"),
          summary: qsTr("Put an OmaShow deck in the picture and step through it a click at a time while you record or stream."),
          also: "presentation slides deck omashow powerpoint pptx keynote key slideshow next previous click talk",
          related: ["sources", "hotkeys"],
          steps: [
              { title: qsTr("Add the presentation"),
                text: qsTr("Press + in Sources and choose Presentation…, then pick an OmaShow deck, or a PowerPoint or Keynote file. Presentation… is there when OmaShow is installed."),
                target: "addSourceButton",
                done: (b, run, ui) => ui.shown("reloadPresentation") },
              { title: qsTr("Step through it"),
                text: qsTr("Press Next on the right, or in the bar under the picture. The points come in a click at a time, as in OmaShow's own show, then the next slide. Previous goes back."),
                target: "slideshowNext" },
              { title: qsTr("Change it in OmaShow"),
                text: qsTr("After you change the deck in OmaShow, press Load again to read it again."),
                target: "reloadPresentation" }
          ],
          finish: qsTr("Your presentation is a source like any other: move it, resize it, or put a camera over it."),
          tips: [
              qsTr("Settings ▸ Hotkeys has Next slide and Previous slide for it, and a stream controller can step it too."),
              qsTr("Movement inside a slide, such as a point flying in, and films on a slide show as still pictures."),
              qsTr("OmaShow 0.2.4 or newer steps a click at a time; an older one shows each slide whole.")
          ] },

        { key: "studios", kind: "task", icon: "layout-grid", level: qsTr("Intermediate"), time: qsTr("About 6 minutes"),
          name: qsTr("Switch between shots"),
          summary: qsTr("Build a few studios, such as a full-screen game and a talking head, and change between them while you record or stream."),
          also: "scenes scene switch cut change shots layouts transitions fade",
          related: ["studios-list", "transitions", "studio-mode"],
          steps: [
              { title: qsTr("Add a studio"),
                text: qsTr("Press + in Studios. A studio is one shot: its own sources, arranged its own way."),
                target: "addStudioButton",
                done: (b, run) => { if (run.step.n === undefined) run.step.n = b.scenes.length; return b.scenes.length > run.step.n } },
              { title: qsTr("Fill it"),
                text: qsTr("The new studio is empty and selected. Add what it should show with + in Sources; the same camera or screen can be in several studios."),
                target: "sourcesDock",
                done: (b) => b.items.length > 0 },
              { title: qsTr("Name your studios"),
                text: qsTr("Double-click a studio to give it a name you will know at a glance, such as Game or Talking."),
                target: "studiosDock" },
              { title: qsTr("Choose how to change"),
                text: qsTr("Cut changes at once; Fade blends over the Duration. + makes a swipe, a slide, a wipe or a stinger video."),
                target: "transitionsDock" },
              { title: qsTr("Switch"),
                text: qsTr("Click another studio. It changes with the transition, in the recording and the stream too."),
                target: "studiosDock",
                done: (b, run) => { if (run.step.s === undefined) run.step.s = b.currentScene; return b.currentScene !== run.step.s } },
              { title: qsTr("Studio mode, when you want it"),
                text: qsTr("Studio mode lets you change a studio in Preview while Program stays on air, then press Take to put it on."),
                target: "studioModeButton" }
          ],
          tips: [
              qsTr("Give studios hotkeys in Settings ▸ Hotkeys to switch without the mouse."),
              qsTr("Tools ▸ Automatic studio switcher… can change studio by itself when you move to another window.")
          ] },

        { key: "save-moments", kind: "task", icon: "rotate-ccw", level: qsTr("Intermediate"), time: qsTr("About 2 minutes"),
          name: qsTr("Save the last few seconds"),
          summary: qsTr("Keep the last moments in memory without recording everything, and save them as a file when something good happens."),
          also: "replay buffer instant replay clip highlight save last seconds",
          related: ["replay-buffer"],
          steps: [
              { title: qsTr("Start the replay buffer"),
                text: qsTr("Choose Record ▸ Start the replay buffer. It keeps the last 20 seconds in memory, and nothing is written yet."),
                target: "",
                done: (b) => b.replayActive },
              { title: qsTr("Save a moment"),
                text: qsTr("Press this button, or Ctrl+Shift+R, just after something you want to keep."),
                target: "saveReplayButton" },
              { title: qsTr("Find it"),
                text: qsTr("It is saved in your recordings folder, named Replay and the date. File ▸ Show recordings opens it."),
                target: "" },
              { title: qsTr("Keep more, or less"),
                text: qsTr("Settings ▸ Replay buffer sets how many seconds it keeps, and how much memory it may use."),
                target: "settingsButton" }
          ],
          tips: [
              qsTr("Settings ▸ Streaming ▸ Keep a replay buffer while streaming starts it with every stream.")
          ] },

        { key: "record-for-editing", kind: "task", icon: "film", level: qsTr("Advanced"), time: qsTr("About 4 minutes"),
          name: qsTr("Record for editing"),
          summary: qsTr("Record in a format that edits smoothly, with your voice and the computer's sound on separate tracks."),
          also: "editing editor apv prores intermediate quality tracks multitrack separate audio",
          related: ["settings-recording", "files-too-big", "mixer"],
          steps: [
              { title: qsTr("Open Settings"),
                text: qsTr("Click the cog at the top right."),
                target: "settingsButton",
                done: (b, run, ui) => ui.shown("settingsNav") },
              { title: qsTr("Go to Recording"),
                text: qsTr("Choose Recording in the list on the left."),
                target: "settingsPage_recording",
                done: (b, run, ui) => ui.shown("settingsRecording") },
              { title: qsTr("Choose an editing encoder"),
                text: qsTr("If APV · graphics card (Vulkan) is in the list, choose it and set Quality to Standard: it edits as smoothly as ProRes. Otherwise use HEVC with Constant quality at 18."),
                target: "recordingEncoder" },
              { title: qsTr("Choose the format"),
                text: qsTr("MKV is safest: it survives a crash. Choose QuickTime (MOV) if your editor needs it."),
                target: "recordingFormat" },
              { title: qsTr("Use more than one sound track"),
                text: qsTr("Tick tracks 1, 2 and 3. Then, in the Audio mixer, open each strip's settings. Leave track 1 on, and also turn on track 2 for your microphone and track 3 for Desktop. Track 1 is then the whole mix."),
                target: "recordingTracks" }
          ],
          tips: [
              qsTr("APV files are large: about a gigabyte a minute at 4K. Check Disk free under Stats before a long session."),
              qsTr("Editors that cannot open APV yet can use the HEVC recording instead.")
          ] },

        // ── Parts of the window ─────────────────────────────────────────
        { key: "preview", kind: "part", icon: "monitor",
          name: qsTr("The picture"),
          summary: qsTr("The big picture in the middle: exactly what is recorded and streamed, and where you arrange it."),
          reach: qsTr("The middle of the window"),
          also: "canvas preview move resize crop rotate arrange position zoom drag",
          target: "studioPreview",
          related: ["sources", "source-settings", "studio-mode"],
          points: [
              qsTr("Click a source to select it; Shift-click to select more."),
              qsTr("Drag to move. Drag a corner to resize; it keeps its shape unless you hold Shift."),
              qsTr("Hold Alt and drag an edge to crop. Drag the round handle above to rotate."),
              qsTr("Right-click a source for its menu: Filters…, Fit to canvas, Centre, Lock, Hide and more.")
          ],
          more: [
              qsTr("Moving snaps to edges and to other sources, with guides; hold Ctrl to stop snapping. Arrow keys nudge 1 pixel, Shift and arrows 10."),
              qsTr("Zoom with − and + at the bottom right of the picture, by pinching on a touchpad, or with Ctrl and the wheel. Click the size between them to fit again. Once zoomed in, scroll or drag with the middle button to move about. Ctrl+0 fits, Ctrl+1 shows 100%."),
              qsTr("View ▸ Lock preview stops anything moving by accident. View ▸ Show overflow shows what lies outside the canvas."),
              qsTr("Copy placement and Paste placement (Ctrl+Alt+C, Ctrl+Alt+V) give one source the exact position and size of another."),
              qsTr("Edit transform… sets position, size, rotation and crop by numbers.")
          ] },

        { key: "studios-list", kind: "part", icon: "layout-grid",
          name: qsTr("Studios"),
          summary: qsTr("Your shots. Each studio has its own sources, arranged its own way; clicking one puts it on."),
          reach: qsTr("Top left"),
          also: "scenes scene list grid",
          target: "studiosDock",
          related: ["studios", "transitions", "studio-mode"],
          points: [
              qsTr("Click a studio to put it on, with the chosen transition."),
              qsTr("+ adds a studio; double-click renames one."),
              qsTr("The button beside + shows studios as a grid instead of a list.")
          ],
          more: [
              qsTr("Right-click a studio to duplicate, reorder or remove it, give it its own transition, open a projector or take a screenshot."),
              qsTr("Show in the live studio puts a studio inside the one that is on, as a source."),
              qsTr("A studio can be a source in another studio: build a frame once and use it everywhere."),
              qsTr("The studios belong to a collection; File ▸ Collection switches between sets of studios.")
          ] },

        { key: "transitions", kind: "part", icon: "refresh-cw",
          name: qsTr("Transitions"),
          summary: qsTr("How one studio changes to the next: a cut, a fade, or a swipe, slide, wipe or stinger video."),
          reach: qsTr("Left, under Studios"),
          also: "fade cut swipe slide wipe stinger transition duration",
          target: "transitionsDock",
          related: ["studios", "studio-mode"],
          points: [
              qsTr("Choose Cut or Fade in the list, and set Duration in milliseconds."),
              qsTr("+ makes a new kind: Fade through colour, Swipe, Cover, Uncover, Slide, Wipe or Stinger.")
          ],
          more: [
              qsTr("⋮ ▸ Properties… changes a transition you made; Add as a quick transition puts it beside Take in studio mode."),
              qsTr("A stinger plays a video over the change; Change studios at sets when the studios swap underneath it, and a track matte can reveal the new studio through a shape."),
              qsTr("Right-click a studio ▸ Transition… gives that studio its own transition into it.")
          ] },

        { key: "sources", kind: "part", icon: "layers",
          name: qsTr("Sources"),
          summary: qsTr("What is in the selected studio: screens, windows, cameras, pictures, text, sound. Higher in the list is in front."),
          reach: qsTr("Left, under Transitions"),
          also: "add source layer display window camera capture device image text colour media file browser audio input phone slideshow presentation omashow slides",
          target: "sourcesDock",
          related: ["source-settings", "preview", "filters"],
          points: [
              qsTr("+ adds a source: Display, Window, Capture device (cameras and capture cards), Media file, Image, Text, Colour, sound and more."),
              qsTr("The eye hides a source without removing it. The lock stops it moving."),
              qsTr("Drag a row up or down, or use the arrows at the top, to put it in front of or behind others."),
              qsTr("Double-click to rename. Right-click for the same menu as in the picture.")
          ],
          more: [
              qsTr("Direct display… and Direct window… capture without the screen-sharing prompt on Hyprland, and Follow focused window shows whichever window you are using."),
              qsTr("Phone (network stream) waits for a phone streaming to this computer, over Wi-Fi or a USB cable: see Use your phone as a camera. Application audio records one app's sound alone."),
              qsTr("Presentation… shows an OmaShow deck (or a PowerPoint or Keynote file) where OmaShow is installed. Next steps to the next click, as in OmaShow's own show; Load again picks up changes made in OmaShow. Movement within a slide and films show as still pictures."),
              qsTr("Group (Ctrl+G) puts several sources in one, to move and hide them together."),
              qsTr("Copy sources, then Paste reference shares the same source; Paste duplicate makes an independent copy."),
              qsTr("A source used in several studios is one source: its settings and filters are the same in all of them.")
          ] },

        { key: "source-settings", kind: "part", icon: "sliders-horizontal",
          name: qsTr("Source settings"),
          summary: qsTr("Everything about the selected source: what it shows, its filters, position, crop and blending."),
          reach: qsTr("Right, at the top (headed Source)"),
          also: "inspector properties settings transform crop opacity blend deinterlace show hide",
          target: "inspectorDock",
          related: ["sources", "filters", "preview"],
          points: [
              qsTr("The first section is the source's own: a camera's picture size, a file to play, the text to show."),
              qsTr("Filters lists its filters; Edit filters… opens them."),
              qsTr("Transform sets position, size and rotation by numbers; Crop trims each edge.")
          ],
          more: [
              qsTr("Picture, on cameras and capture cards, has the device's own controls such as brightness and focus, and its colour range."),
              qsTr("Show and hide plays a transition when a source is shown or hidden."),
              qsTr("Blending sets Opacity, a blend mode and the scaling filter for this source."),
              qsTr("Deinterlacing is for interlaced video from older cameras and capture cards.")
          ] },

        { key: "filters", kind: "part", icon: "sliders-horizontal",
          name: qsTr("Filters"),
          summary: qsTr("Change how one source looks: its colour, sharpness, size or shape, or take away the background behind you, with or without a green screen."),
          reach: qsTr("Right-click a source ▸ Filters…, or Edit filters… in the source's settings on the right"),
          also: "effects colour color correction green screen chroma key blur crop lut mask sharpen remove background ai person cut out light wrap glow edges grade wheels lift gamma gain scopes waveform vectorscope",
          target: "sourcesDock",
          related: ["green-screen", "remove-background", "grade", "dropped-frames"],
          points: [
              qsTr("Press Add filter and choose one from its groups: Keying, Colour, Size and position, Look or Timing. The picture on the right shows the result as you change it."),
              qsTr("Filters run from the top of the list to the bottom. Drag one up or down to change the order, or right-click it for Move up and Move down."),
              qsTr("The eye beside a filter turns it off and on, to compare with and without it, keeping its settings. The bin, or the Delete key, takes it out; Undo brings it back."),
              qsTr("⋮ ▸ Copy all filters, then Paste filters on another source, puts the same filters there.")
          ],
          more: [
              qsTr("Filters belong to the source, so a source used in several studios looks the same in all of them."),
              qsTr("Chroma key finds the backdrop's colour in the picture by itself. Show matte shows what is kept in white and what is taken out in black; Pick from preview takes the colour from where you click."),
              qsTr("Remove background (AI) finds the people in the picture and takes the rest away, or with Background set to Blur keeps it out of focus behind them, with no green screen. It knows people only. Fast is lightest on the graphics card; raise Tighten edges if a rim of the background shows."),
              qsTr("Light wrap, put after a key, brings whatever is behind the source a little way over its edges, so a keyed person looks lit by the scene. Add light brightens the edges; Blend colour takes them towards the colours behind, which suits dark scenes. It needs what is behind, so while it is selected the filters preview shows the studio (In the studio)."),
              qsTr("Every number is a slider. Click one and use the arrow keys to move it a step at a time; the arrow beside it puts it back."),
              qsTr("Colour wheels grades as a grading tool does: Lift, Gamma, Gain and Offset each push their part of the picture towards a colour and set its level, with Contrast, Pivot and Saturation beneath."),
              qsTr("Scopes, under the preview, show the picture as a waveform, an RGB parade, a vectorscope or a histogram. They come on by themselves for the colour filters."),
              qsTr("Apply LUT reads .cube files. Image mask/blend uses a picture's brightness or transparency to cut out or blend the source."),
              qsTr("Each filter runs on the graphics card every frame. If a set of filters takes too much of each frame, the window says so in amber; see Dropped frames."),
              qsTr("Render delay and Video delay hold the picture back, to line it up with sound that arrives late.")
          ] },

        { key: "mixer", kind: "part", icon: "audio-lines",
          name: qsTr("Audio mixer"),
          summary: qsTr("Every sound going into the recording and stream, with how loud it is and controls to change it."),
          reach: qsTr("Right, under Source settings"),
          also: "audio sound volume mute level meter microphone desktop monitor headphones solo tracks balance",
          target: "mixerDock",
          related: ["audio-filters", "no-sound", "echo", "out-of-sync"],
          points: [
              qsTr("The mixer shows only the sound you add. Click + in Sources and choose your microphone by its name, or Desktop sound for the computer's sound. A camera brings only its picture; its microphone is in that list too."),
              qsTr("X on a Desktop, Microphone or Sound for every studio strip removes that sound, and it stops being captured. For anything else, right-click it in Sources and choose Remove."),
              qsTr("The meters: green is good, yellow is loud, red is too loud and will distort."),
              qsTr("The slider and the dB box set the level. The speaker mutes."),
              qsTr("The waveform button opens a strip's audio filters; it says FX when the meters run up and down.")
          ],
          more: [
              qsTr("The sliders button opens a strip's settings: monitoring, mono, balance, sync offset, which recording tracks it goes to, and push-to-talk."),
              qsTr("Monitoring lets you hear a strip in your headphones: choose the output under Monitoring output at the bottom, then turn on monitoring in the strip's settings. Solo hears one strip alone."),
              qsTr("+ in Sources → Sound for every studio adds microphones or outputs that stay in every studio. Channels shows strips you hid."),
              qsTr("Meters sets sample or true peak, how fast the meters fall, and whether strips run across or up and down."),
              qsTr("Active sources only hides strips for sources not in the studio on air.")
          ] },

        { key: "audio-filters", kind: "part", icon: "audio-waveform",
          name: qsTr("Audio filters"),
          summary: qsTr("Clean up and shape a sound: take out noise, even out the level, stop it clipping."),
          reach: qsTr("The waveform button (FX with vertical meters) on a strip in the Audio mixer"),
          also: "noise suppression rnnoise gate compressor limiter eq equaliser de-esser gain expander voice",
          target: "mixerDock",
          related: ["mixer", "camera-mic"],
          points: [
              qsTr("For a voice, add in this order: RNNoise speech suppression, Compressor, then Limiter."),
              qsTr("Press Add filter and choose from Level, Dynamics, Tone, Noise or Timing. Filters run from the top down: drag one to change the order, the eye turns it off, the bin takes it out.")
          ],
          more: [
              qsTr("RNNoise speech suppression is for speech only; Spectral noise suppression suits music and other sounds."),
              qsTr("Noise gate silences the microphone between words; Expander turns quiet sounds down more gently."),
              qsTr("Compressor makes loud and quiet parts closer; Upward compressor lifts the quiet parts; Limiter is a ceiling nothing goes over."),
              qsTr("3-band EQ shapes low, middle and high; De-esser softens sharp s sounds; Gain adds or takes away level; Delay holds the sound back; Invert polarity fixes two microphones cancelling out.")
          ] },

        { key: "stats", kind: "part", icon: "activity",
          name: qsTr("Stats"),
          summary: qsTr("How the computer is keeping up: processor, graphics card, disk space and every output."),
          reach: qsTr("View ▸ Docks ▸ Stats (closed at first)"),
          also: "statistics performance cpu gpu disk space frames dropped late render time encode",
          target: "statsDock",
          related: ["dropped-frames", "files-too-big"],
          points: [
              qsTr("Frames per second should match your frame rate."),
              qsTr("Late frames, Dropped frames and Sending behind should stay at or near zero."),
              qsTr("Time left says how long you can record before the disk is full.")
          ],
          more: [
              qsTr("Render time is how long a frame takes to draw; it must stay under one frame: 16.7 ms at 60 frames a second, 33.3 ms at 30."),
              qsTr("Encode time is per output; Sending behind is how far a stream's upload has fallen behind."),
              qsTr("The status bar at the bottom shows the canvas size and the graphics card drawing the picture, and says when a laptop is on battery.")
          ] },

        { key: "studio-mode", kind: "part", icon: "clapperboard",
          name: qsTr("Studio mode"),
          summary: qsTr("Change a studio in Preview while the Program stays on air, then put it on with Take."),
          reach: qsTr("Studio mode in the toolbar, or View ▸ Studio mode"),
          also: "preview program take t-bar quick transition studio mode",
          target: "studioModeButton",
          related: ["studios", "transitions"],
          points: [
              qsTr("The left picture is Preview: what you are getting ready. The right is Program: what is on air."),
              qsTr("Click a studio to put it in Preview; press Take to put it on."),
              qsTr("Turn Studio mode off to go back to one picture.")
          ],
          more: [
              qsTr("Drag the T-bar to mix to the next studio by hand."),
              qsTr("Quick transition buttons beside Take use a transition at once; add them from Transitions ▸ ⋮ ▸ Add as a quick transition."),
              qsTr("Swap puts the old Program in Preview after a take; Copy and Copy sources keep a copy of what went on air.")
          ] },

        { key: "layout", kind: "part", icon: "app-window",
          name: qsTr("Arranging the window"),
          summary: qsTr("The panels round the picture can be folded, moved, closed and brought back."),
          reach: qsTr("Each panel's title and ⋮ menu, and View ▸ Docks"),
          also: "docks panels layout reset hide show move fold browser dock",
          related: ["stats"],
          points: [
              qsTr("Click a panel's title to fold it away; click again to open it."),
              qsTr("⋮ ▸ Move to the other side, Move up, Move down or Close."),
              qsTr("View ▸ Docks brings back a closed panel; Reset docks puts everything back as it was.")
          ],
          more: [
              qsTr("Drag the line between two panels to share the space differently."),
              qsTr("View ▸ Docks ▸ Lock docks stops panels moving by accident."),
              qsTr("Custom browser docks… adds a web page as a panel, such as a chat or an alerts page.")
          ] },

        { key: "settings-video", kind: "part", icon: "monitor",
          name: qsTr("Settings: Video"),
          summary: qsTr("The size of the picture and how many frames a second it has, for everything that goes out."),
          reach: qsTr("Settings ▸ Video"),
          also: "canvas resolution size frame rate fps 60 30 colour space hdr scaling",
          target: "settingsButton",
          related: ["dropped-frames", "settings-recording"],
          points: [
              qsTr("Canvas is the picture's size; until you choose one it follows the first display you capture."),
              qsTr("Frame rate: 60 is smoothest; 30 is half the work for the computer and fine for talking and slides.")
          ],
          more: [
              qsTr("Scaling sets how pictures are resized to fit: Area is sharp and quick; Lanczos is sharpest for big reductions."),
              qsTr("Colour space Rec. 709 is right for nearly everything; Rec. 2100 (PQ or HLG) makes HDR, with SDR white and HDR peak setting the levels."),
              qsTr("Colour format chooses the bit depth; 10-bit avoids banding in smooth gradients if the encoder supports it."),
              qsTr("Range Limited is the video standard; use Full only when the next program expects it.")
          ] },

        { key: "settings-audio", kind: "part", icon: "volume-2",
          name: qsTr("Settings: Audio"),
          summary: qsTr("The sound format for everything that goes out."),
          reach: qsTr("Settings ▸ Audio"),
          also: "sample rate 48 khz stereo mono channels latency",
          target: "settingsButton",
          related: ["mixer"],
          points: [
              qsTr("48 kHz and Stereo suit nearly everything."),
              qsTr("Which microphone to use is chosen with + in Sources, not here.")
          ],
          more: [
              qsTr("Low latency makes monitoring quicker at the cost of more work for the computer."),
              qsTr("The sound format cannot change while anything is recording, streaming or running.")
          ] },

        { key: "settings-recording", kind: "part", icon: "circle",
          name: qsTr("Settings: Recording"),
          summary: qsTr("How recordings are made: the encoder, the quality, the file type and where they go."),
          reach: qsTr("Settings ▸ Recording"),
          also: "encoder codec quality bitrate format mkv mp4 mov apv hevc h264 av1 folder file name tracks",
          target: "settingsButton",
          related: ["record-for-editing", "files-too-big", "cant-find-recording", "faq-encoder"],
          points: [
              qsTr("Encoder: Best available is right for most people. Only encoders that work on this computer are listed."),
              qsTr("Quality: lower numbers look better and make bigger files. 23 is a good balance; 18 is near perfect."),
              qsTr("Folder: where recordings go. Choose… picks another one."),
              qsTr("Format MKV survives a crash; tick Also make an MP4 copy when it ends for a file that plays everywhere.")
          ],
          more: [
              qsTr("Encoders on the graphics card (NVIDIA, VA-API, Vulkan) leave the processor free; processor encoders are slower but can be smaller."),
              qsTr("APV is for editing: very high quality, large files, quick to edit. Its Quality is Light, Standard or High."),
              qsTr("Rate control: Constant quality for recording; Constant or Variable bitrate when the file size must be predictable."),
              qsTr("Audio tracks puts up to six tracks in the file; each mixer strip chooses its tracks in its settings."),
              qsTr("New file every … minutes or MB splits a long recording. Advanced shows keyframes, presets and encoder options."),
              qsTr("H.264 and HEVC on the graphics card hardly slow the drawing; APV runs on the same cores as the studio and its filters, so at 4K it can cost frames. See Which encoder should I record with?")
          ] },

        { key: "settings-streaming", kind: "part", icon: "radio",
          name: qsTr("Settings: Streaming"),
          summary: qsTr("Where you go live: one stream for each service, and all the ones ticked go out together."),
          reach: qsTr("Settings ▸ Streaming, or Stream ▸ Stream settings…"),
          also: "stream key server twitch youtube facebook restream bitrate multistream reconnect",
          target: "settingsButton",
          related: ["go-live", "stream-fails"],
          points: [
              qsTr("Add a stream… asks for the service and the stream key from its dashboard."),
              qsTr("Tick the streams to use; Go live starts them all."),
              qsTr("Keep to the service's limits is on, so the bitrate and size stay within what it accepts.")
          ],
          more: [
              qsTr("Bitrate: 6000 kb/s suits 1080p at 60 on Twitch. Your upload should be at least half as fast again."),
              qsTr("If it drops: Reconnect tries again by itself; Lower the bitrate when the network can't keep up eases off instead of dropping frames, on the encoders that can change their bitrate while they run; the sheet says when yours cannot."),
              qsTr("Each stream can have its own encoder, size and sound track."),
              qsTr("Record while streaming and Stop recording when the stream ends tie the recording to the stream."),
              qsTr("Stream keys are kept in the system keyring. Exported profiles leave them out.")
          ] },

        { key: "replay-buffer", kind: "part", icon: "rotate-ccw",
          name: qsTr("Replay buffer"),
          summary: qsTr("Keeps the last moments in memory, to save as a file when you choose."),
          reach: qsTr("Record ▸ Start the replay buffer; Settings ▸ Replay buffer"),
          also: "replay instant clip save last seconds",
          target: "settingsButton",
          related: ["save-moments"],
          points: [
              qsTr("Start it from the Record menu; save with Ctrl+Shift+R or the button by Record."),
              qsTr("Keep sets how many seconds, and how much memory it may use.")
          ],
          more: [
              qsTr("Replays are saved in the recordings folder, in the recording format, named with Before and After round the date."),
              qsTr("The bar or tray icon's menu has Save replay too.")
          ] },

        { key: "virtual-camera", kind: "part", icon: "camera",
          name: qsTr("Virtual camera"),
          summary: qsTr("Use the picture as a camera in video calls and other apps."),
          reach: qsTr("Stream ▸ Start the virtual camera, or the camera button in the toolbar"),
          also: "virtual camera webcam video call meet zoom teams discord",
          target: "settingsButton",
          related: ["studios"],
          points: [
              qsTr("Start it, then choose OmaBroadcast Camera as the camera in the other app."),
              qsTr("Settings ▸ Virtual camera ▸ Shows picks what it sends: the Program, the Preview, one studio or one source.")
          ] },

        { key: "hotkeys", kind: "part", icon: "keyboard",
          name: qsTr("Hotkeys"),
          summary: qsTr("Keys for anything: switching studios, muting, recording, saving a replay."),
          reach: qsTr("Settings ▸ Hotkeys, or Tools ▸ Hotkeys…"),
          also: "keyboard shortcuts keys global bind push to talk",
          target: "settingsButton",
          related: ["studios"],
          points: [
              qsTr("Find an action, then give it a key."),
              qsTr("Keys work chooses whether they work always, only in OmaBroadcast, or only while you are in another app.")
          ],
          more: [
              qsTr("Keys that work in other apps need Apply global shortcuts."),
              qsTr("Push-to-talk and push-to-mute are set per mixer strip, in its settings.")
          ] },

        { key: "launch-window", kind: "part", icon: "info",
          name: qsTr("The launch window"),
          summary: qsTr("The small window at each start: the version, a quotation and OK."),
          reach: qsTr("Each time OmaBroadcast starts"),
          also: "launch window start splash welcome version number quote licence ok",
          related: ["crashed"],
          points: [
              qsTr("OK or Return opens OmaBroadcast; nothing else starts until you do. Escape, or closing it, leaves."),
              qsTr("The version is the number to give in a bug report. Build & licence has the licences."),
              qsTr("Click the name under a quotation to see where it comes from."),
              qsTr("It is left out in safe mode, after a crash and on a restart. A computer that starts OmaBroadcast with no one there can set OMABROADCAST_NO_LAUNCH_WINDOW=1 in its environment.")
          ] },

        { key: "prompter", kind: "part", icon: "file-text",
          name: qsTr("Prompter"),
          summary: qsTr("Your script in a window of its own, to read beside the camera or over the picture. It is never recorded."),
          reach: qsTr("Tools ▸ Prompter"),
          also: "prompter teleprompter autocue script read words scroll camera presenter",
          related: ["settings-general"],
          points: [
              qsTr("Load… takes a text or Markdown file, and opens it again next time. Word documents need saving as text first."),
              qsTr("Drag the bar along the top to move the window, and an edge or corner to resize it. On Hyprland it stays above other windows, so it can sit over the picture."),
              qsTr("Play scrolls the words past the marks at each side. Space pauses, Up and Down change the speed, Home goes back to the start."),
              qsTr("The window is never part of the picture, so recordings and streams cannot show it."),
              qsTr("On Hyprland it is also kept out of screen captures, where it shows as a black box: put it on a screen you are not capturing."),
              qsTr("Hotkeys can start and pause it: see Tools ▸ Hotkeys.")
          ] },

        { key: "playback", kind: "part", icon: "film",
          name: qsTr("Play last recording"),
          summary: qsTr("The last recording in a window of its own, to check its picture and sound before you send it on."),
          reach: qsTr("Tools ▸ Play last recording"),
          also: "playback play back check qc quality control review last recording meters peak shuttle jkl frame step",
          related: ["files", "mixer"],
          points: [
              qsTr("It opens the recording you made last, or the newest one in the recordings folder, and moves to each new one as it is saved."),
              qsTr("Space plays and stops. L plays forward, and again for 2 and 4 times as fast; J goes backward the same way, as still pictures without sound; K stops."),
              qsTr("Left and Right move one frame, with Shift one second. Home and End go to the start and the end, and the bar along the bottom drags through it."),
              qsTr("A meter for each channel shows the sound as it plays; Peak is the loudest moment so far, red if it reached 0 dB. Click it to start again."),
              qsTr("The info button (I) lists what the file is: its format, length, size, picture and each sound track."),
              qsTr("The sound plays on your monitoring output, or the desktop's if none is chosen. While desktop sound is being recorded or streamed, it stays silent there, so the check cannot get into the recording."),
              qsTr("On Hyprland it floats, and is kept out of screen captures, where it shows as a black box.")
          ] },

        { key: "settings-general", kind: "part", icon: "settings",
          name: qsTr("Settings: General"),
          summary: qsTr("The graphics card, the bar or tray icon, and whether to ask before starting and stopping."),
          reach: qsTr("Settings ▸ General"),
          also: "gpu graphics card bar omarchy tray confirm hide from capture",
          target: "settingsButton",
          related: ["dropped-frames"],
          points: [
              qsTr("Graphics: Automatic chooses the card; pick one to make sure, on a computer with two."),
              qsTr("Confirm asks before a recording or stream starts or stops, so a stray click cannot end it.")
          ],
          more: [
              qsTr("Hide OmaBroadcast from screen capture keeps its own window out of a display you capture, so the preview does not repeat inside itself (Hyprland). Hyprland shows the window black instead, in screenshots too, so it is off unless you turn it on."),
              qsTr("On Omarchy, OmaBroadcast puts its own icon in the bar: click it to show or hide the window, right-click it to start and stop recording and streaming. Move or remove it as with any bar icon, or turn it off here. Elsewhere a tray icon does the same. Either can keep OmaBroadcast running with its window closed, and start it hidden.")
          ] },

        { key: "setup", kind: "part", icon: "list-checks",
          name: qsTr("Set up for…"),
          summary: qsTr("Answers a few questions, tests this computer and makes a set-up that suits what you are making."),
          reach: qsTr("Tools ▸ Set up for… (also offered the first time OmaBroadcast opens)"),
          also: "wizard setup auto configure first time start",
          related: ["first-recording", "go-live", "profiles"],
          points: [
              qsTr("Choose Streaming, Recording, Tutorials and screencasts, Recording for editing, Game replays, Webcam and video calls, or Light recording."),
              qsTr("Pick the display whose size the picture takes, and a camera and a microphone if you want them; for streaming, the service and key."),
              qsTr("The starter studios hold only what you chose. Tick Show this display in the studios to have your screen in them."),
              qsTr("It tests each encoder and your upload, then shows what it will set and why.")
          ],
          more: [
              qsTr("Create makes a new profile and a new collection; nothing you already have changes."),
              qsTr("A microphone you pick is added to Sources beside the camera. Automatic follows the desktop's default input and shows in the mixer only."),
              qsTr("With nothing chosen to show, you get one empty studio to fill yourself."),
              qsTr("The upload test is a short stream that never goes live.")
          ] },

        { key: "profiles", kind: "part", icon: "folder-open",
          name: qsTr("Profiles and collections"),
          summary: qsTr("A profile is a set of video, recording, audio and streaming settings. A collection is a set of studios and sources."),
          reach: qsTr("File ▸ Profiles… and File ▸ Collection"),
          also: "profile collection import export switch backup",
          related: ["setup"],
          points: [
              qsTr("Switch between them for different jobs: a streaming profile and a recording profile, say."),
              qsTr("The status bar shows the profile and collection in use.")
          ],
          more: [
              qsTr("Collections can be duplicated, imported and exported, to move a set-up to another computer."),
              qsTr("Exported profiles leave out stream keys.")
          ] },

        { key: "tools", kind: "part", icon: "settings",
          name: qsTr("Tools"),
          summary: qsTr("Timers, automatic switching, remote control and scripts."),
          reach: qsTr("The Tools menu"),
          also: "output timer automatic switcher remote control websocket stream deck scripts automation",
          related: ["studios", "hotkeys"],
          points: [
              qsTr("Output timer… stops a recording or ends a stream after a set time."),
              qsTr("Automatic studio switcher… changes studio by itself when you move to another window (Hyprland).")
          ],
          more: [
              qsTr("Remote control… lets other apps and control panels drive OmaBroadcast over the network, with a password, using version 5 of the common remote-control protocol on port 4455."),
              qsTr("Scripts… runs your own actions from hotkeys; scripts cannot reach files or the network."),
              qsTr("omabroadcast ctl in a terminal starts and stops recording, streaming and more.")
          ] },

        { key: "projectors", kind: "part", icon: "maximize-2",
          name: qsTr("Projectors and multiview"),
          summary: qsTr("Show the program, the preview, a studio or a source in a window of its own, such as on a second screen."),
          reach: qsTr("View ▸ Open a projector, or right-click a studio or source ▸ Open a projector"),
          also: "projector fullscreen second monitor multiview confidence",
          related: ["studio-mode"],
          points: [
              qsTr("Program shows what is on air; Preview what is ready in studio mode."),
              qsTr("Multiview shows every studio at once, to see what is coming.")
          ] },

        { key: "files", kind: "part", icon: "file-text",
          name: qsTr("Recordings and files"),
          summary: qsTr("Finding recordings, making MP4 copies, and fixing sources whose files have moved."),
          reach: qsTr("The File menu"),
          also: "show recordings remux mp4 missing files relink moved",
          related: ["cant-find-recording", "settings-recording"],
          points: [
              qsTr("File ▸ Show recordings opens the recordings folder."),
              qsTr("File ▸ Remux recordings… makes an MP4 copy of a recording quickly, without changing the picture."),
              qsTr("File ▸ Find missing files… finds pictures and videos that have moved, and points the sources at them again.")
          ] },

        // ── When something goes wrong ──────────────────────────────────
        { key: "dropped-frames", kind: "fix", icon: "activity",
          name: qsTr("Dropped frames or stutter"),
          summary: qsTr("The recording or stream jerks, or Stats counts dropped frames. The computer is not keeping up with everything asked of it."),
          reach: qsTr("Stats shows Late frames, Render time and Dropped frames"),
          also: "lag stutter choppy slow skipping late frames performance fps jerky",
          target: "statsDock",
          related: ["filters", "stats", "settings-video", "faq-4k60-ai", "faq-capture-60"],
          points: [
              qsTr("Close games and other apps that use the graphics card while you record."),
              qsTr("Turn off filters you do not need, starting with any that show a warning in their Filters window."),
              qsTr("Using Remove background (AI)? Set its Quality to Fast; it is the heaviest filter there is."),
              qsTr("Use 30 frames a second instead of 60 (Settings ▸ Video ▸ Frame rate)."),
              qsTr("Use a smaller canvas, such as 1920 × 1080 instead of 3840 × 2160 (Settings ▸ Video)."),
              qsTr("Choose a different encoder in Settings ▸ Recording or Settings ▸ Streaming, such as one on another graphics card.")
          ],
          more: [
              qsTr("Late frames means the picture took longer to draw than one frame lasts. Render time shows how long it takes; it must stay under 16.7 ms at 60 frames a second, or 33.3 ms at 30."),
              qsTr("Dropped frames under Outputs means the encoder or the network fell behind, not the drawing: lower the bitrate, or use a lighter encoder."),
              qsTr("Sending behind means your upload is too slow for the bitrate. Tools ▸ Set up for… measures your upload and suggests one."),
              qsTr("On a laptop, plug in the charger: on battery the graphics card may slow down to save power."),
              qsTr("A camera or capture card at 4K 60 in MJPEG needs several processor cores just to unpack its pictures: try 1080p 60, or 4K 30."),
              qsTr("At 4K 60 with Remove background (AI), most laptops are too slow: see What computer do I need for 4K at 60.")
          ] },

        { key: "no-sound", kind: "fix", icon: "volume-x",
          name: qsTr("No sound, or the wrong microphone"),
          summary: qsTr("The meters do not move, or the recording is silent."),
          also: "silent mute audio missing microphone not working no audio quiet",
          target: "mixerDock",
          related: ["mixer", "echo"],
          points: [
              qsTr("In the Audio mixer, check there is a strip for the microphone you speak into. If not, click + in Sources and choose it under Microphone or audio input. It sounds only in studios it is in; to have it in all of them, use Sound for every studio…"),
              qsTr("Check the strip is not muted (a crossed-out speaker) and its slider is not at the bottom."),
              qsTr("Speak, and watch its meter. No movement means no sound is arriving: check the cable, and that the microphone is not muted on the microphone itself."),
              qsTr("Channels at the top of the mixer shows strips that were hidden.")
          ],
          more: [
              qsTr("If the meters move but the recording is silent, check Settings ▸ Recording ▸ Audio tracks includes track 1, and the strip's settings send it to track 1."),
              qsTr("Hearing nothing yourself is normal: choose an output under Monitoring output at the bottom of the mixer, then turn on monitoring in the strip's settings, or press Solo."),
              qsTr("A source's own sound, such as a video file, has its own strip; Active sources only hides strips for sources not on air.")
          ] },

        { key: "echo", kind: "fix", icon: "headphones",
          name: qsTr("Echo, or sound heard twice"),
          summary: qsTr("Your voice or the computer's sound is in the recording twice, or repeats."),
          also: "echo double feedback twice repeat loop howl",
          target: "mixerDock",
          related: ["mixer", "no-sound"],
          points: [
              qsTr("Wear headphones: speakers are picked up by the microphone."),
              qsTr("If you listen to yourself through speakers or an output that Desktop also records, your voice is recorded twice: choose headphones under Monitoring output, or set the microphone's monitoring off."),
              qsTr("Remove extra sources for the same sound, such as a second strip for a microphone you already have.")
          ],
          more: [
              qsTr("In a video call the other people's sound comes through Desktop; that is right, but your own voice should only come from your microphone's strip."),
              qsTr("Application audio records a single app, which avoids catching everything else on Desktop.")
          ] },

        { key: "out-of-sync", kind: "fix", icon: "audio-waveform",
          name: qsTr("Sound and picture out of step"),
          summary: qsTr("Lips move before or after the words, or a clap is heard early or late."),
          also: "sync lip sync delay offset latency audio late video late",
          target: "mixerDock",
          related: ["mixer", "filters"],
          points: [
              qsTr("Clap in front of the camera and watch where the sound lands in the recording."),
              qsTr("Sound early: open the microphone strip's settings and raise Sync offset, in milliseconds, until it lines up."),
              qsTr("Picture early: add a Video delay filter to the camera.")
          ],
          more: [
              qsTr("Cameras and capture cards often show the picture 50 to 150 ms late, so the voice needs holding back to match."),
              qsTr("Wireless and Bluetooth microphones add their own delay; a cable avoids it.")
          ] },

        { key: "black-window", kind: "fix", icon: "monitor",
          name: qsTr("A window or screen shows black"),
          summary: qsTr("A display or window is added, but it shows nothing."),
          also: "black screen capture empty blank window not showing portal screenshot omabroadcast hidden",
          target: "sourcesDock",
          related: ["sources", "crashed"],
          points: [
              qsTr("Right-click it and choose Choose window… or Choose display…, and share it again."),
              qsTr("Some apps stop drawing when minimised or on another workspace: keep the window open."),
              qsTr("On Hyprland, try Direct window… or Direct display… from + in Sources."),
              qsTr("OmaBroadcast's own window black in a capture or a screenshot? Settings ▸ General ▸ Hide OmaBroadcast from screen capture is on: untick it.")
          ],
          more: [
              qsTr("Help ▸ Repair… clears screen-sharing choices whose sources are gone. For a source still here, right-click it and choose again."),
              qsTr("Apps with protected video, such as some streaming services in a browser, show black on purpose.")
          ] },

        { key: "not-chosen", kind: "fix", icon: "monitor",
          name: qsTr("A screen or window says “not chosen yet”"),
          summary: qsTr("OmaBroadcast has no remembered choice for it, so it waits for you rather than opening the share picker by itself."),
          also: "not chosen yet share picker screen display window asks choose portal remembered",
          target: "sourcesDock",
          related: ["black-window"],
          points: [
              qsTr("Right-click the source and use Choose display… or Choose window…, then pick what to share. It is remembered, and comes back by itself next time."),
              qsTr("The desktop can forget a choice. When it has, the source waits here again; the picture is not lost, only the choice."),
              qsTr("If the picker still opens by itself, what it remembered may be gone: a monitor unplugged, or a window closed. Choose again.")
          ] },

        { key: "no-camera", kind: "fix", icon: "camera",
          name: qsTr("The camera does not show"),
          summary: qsTr("The camera or capture card is not in the list, or shows nothing."),
          also: "camera webcam capture card not found black no picture usb hdmi",
          target: "sourcesDock",
          related: ["camera-mic", "source-settings"],
          points: [
              qsTr("Close other apps that use the camera, such as video calls: a camera works in one app at a time."),
              qsTr("Unplug it and plug it in again; a camera source starts by itself when it comes back."),
              qsTr("Choose a smaller picture size in its settings on the right: some cameras cannot send their biggest size over USB 2."),
              qsTr("For a capture card, check the camera or console is on and sending, and the HDMI cable is in.")
          ],
          more: [
              qsTr("4K capture cards need a USB 3 port, usually the blue one or one marked SS."),
              qsTr("If the picture is too dark, too pale or the wrong colours, Picture ▸ Defaults resets the device's own controls, and Colour sets the range and matrix by hand.")
          ] },

        { key: "stream-fails", kind: "fix", icon: "radio",
          name: qsTr("The stream will not start, or drops"),
          summary: qsTr("Go live fails, nothing reaches the channel, or the stream keeps disconnecting."),
          also: "stream failed connect error key rtmp disconnect offline",
          target: "streamButton",
          related: ["go-live", "settings-streaming", "dropped-frames"],
          points: [
              qsTr("Copy the stream key again from the service's dashboard: a key changes if it is reset."),
              qsTr("Check the stream is ticked in Settings ▸ Streaming."),
              qsTr("Choose an automatic server, or the plain RTMP one if the encrypted one is blocked."),
              qsTr("Help ▸ Log says why the service refused it.")
          ],
          more: [
              qsTr("If it keeps dropping, lower the bitrate, or turn on Lower the bitrate when the network can't keep up."),
              qsTr("A wired connection is steadier than Wi-Fi for streaming."),
              qsTr("Restream sends to several services at once from one upload, if your upload is too slow for several streams.")
          ] },

        { key: "cant-find-recording", kind: "fix", icon: "folder-open",
          name: qsTr("Where is my recording?"),
          summary: qsTr("A recording finished but you cannot find the file, or it will not play."),
          also: "find file folder location saved where mkv play open",
          related: ["files", "settings-recording"],
          points: [
              qsTr("File ▸ Show recordings opens the folder."),
              qsTr("Unless you chose another in Settings ▸ Recording ▸ Folder, it is the OmaBroadcast folder in your Videos."),
              qsTr("MKV files play in most players, such as mpv and VLC. For other apps, File ▸ Remux recordings… makes an MP4 copy.")
          ] },

        { key: "files-too-big", kind: "fix", icon: "file-text",
          name: qsTr("Recordings are too big"),
          summary: qsTr("Files fill the disk too quickly."),
          also: "file size large disk space bitrate smaller compress",
          related: ["settings-recording", "stats"],
          points: [
              qsTr("Raise Quality in Settings ▸ Recording a few steps, such as from 23 to 27: each step makes the file smaller."),
              qsTr("Choose HEVC or AV1 instead of H.264: the same quality in a smaller file."),
              qsTr("Record a smaller Size, or at 30 frames a second.")
          ],
          more: [
              qsTr("APV is large on purpose: it is for editing. Use it only when you need it."),
              qsTr("Stats ▸ Time left shows how long you can record before the disk is full.")
          ] },

        { key: "crashed", kind: "fix", icon: "life-buoy",
          name: qsTr("It crashed, or will not start properly"),
          summary: qsTr("OmaBroadcast closed by itself, or something is wrong every time it opens."),
          also: "crash crashed closed quit freeze hang safe mode repair log broken reset",
          related: ["cant-find-recording"],
          points: [
              qsTr("Next time it opens it offers the log and safe mode, which starts without restoring screen captures, without filter files and without the graphics card, so you can find what went wrong."),
              qsTr("Help ▸ Repair… sets aside anything that cannot be read, resets the layout and clears screen-sharing choices whose sources are gone. Nothing is deleted."),
              qsTr("A recording in MKV, the default, keeps what was recorded up to the crash.")
          ],
          more: [
              qsTr("Help ▸ Log shows what happened; copy it into a bug report."),
              qsTr("If one source causes the trouble, safe mode lets you remove it, then start normally."),
              qsTr("Save the log… in that window keeps the last run's log in a file for a bug report. Nothing is sent anywhere.")
          ] },

        // Questions people ask: the short answer, then the detail and figures.
        { key: "faq-4k60-ai", kind: "faq", icon: "monitor",
          name: qsTr("What computer do I need for 4K at 60 with the AI filters?"),
          summary: qsTr("A mid-range desktop graphics card or better. Built-in graphics and entry laptop cards manage 1080p at 60, or 4K at 30, but not 4K at 60 with Remove background and Light wrap."),
          also: "requirements specs specification hardware graphics card gpu 4k 60 fps ai remove background light wrap laptop nvidia amd intel recommended minimum",
          related: ["remove-background", "dropped-frames", "settings-video", "faq-capture-60"],
          points: [
              qsTr("At 60 frames a second each frame has 16.7 ms. Drawing the studio, the filters and getting the picture ready for the encoder all have to fit in that, every frame."),
              qsTr("The work grows with the graphics card's computing power and memory speed. For 4K at 60 with room to spare, aim for about 10 TFLOPS and 250 GB/s or more."),
              qsTr("Not enough? Use a 1920 × 1080 canvas (Settings ▸ Video), or 30 frames a second: either fits comfortably on modest hardware.")
          ],
          tables: [
              { title: qsTr("Measured: 4K at 60, Remove background (Fast, spread over 2 frames) and Light wrap"),
                columns: [qsTr("Graphics card"), qsTr("Each frame"), qsTr("Result")],
                rows: [[qsTr("RTX 5060 Ti (desktop)"), qsTr("about 5–6 ms"), qsTr("Easy, lots of room")],
                       [qsTr("Arc B580 (desktop)"), qsTr("about 7 ms"), qsTr("Easy, lots of room")],
                       [qsTr("RTX 3050 Laptop"), qsTr("about 17–19 ms"), qsTr("Just short")],
                       [qsTr("Radeon 780M (built-in)"), qsTr("over 30 ms"), qsTr("No: drawing 4K alone takes 15.5 ms")]] },
              { title: qsTr("Recommended for 4K at 60 with the AI filters"),
                columns: [qsTr("Part"), qsTr("Minimum"), qsTr("Comfortable")],
                rows: [[qsTr("NVIDIA graphics"), qsTr("RTX 4060 / 5060 desktop, or RTX 4070 Laptop"), qsTr("RTX 4060 Ti / 5060 Ti or above")],
                       [qsTr("AMD graphics"), qsTr("RX 7600 / 9060"), qsTr("RX 7700 XT / 9060 XT or above")],
                       [qsTr("Intel graphics"), qsTr("Arc B580"), qsTr("Arc B580 or above")],
                       [qsTr("Graphics memory"), qsTr("8 GB"), qsTr("8 GB or more")],
                       [qsTr("Processor"), qsTr("6 modern cores"), qsTr("8 cores (a 4K 60 MJPEG camera uses 2 to 4)")],
                       [qsTr("Memory"), qsTr("16 GB"), qsTr("32 GB")],
                       [qsTr("Encoder"), qsTr("H.264 or HEVC on the graphics card"), qsTr("The same; APV at 4K 60 needs much spare graphics power")],
                       [qsTr("Capture"), qsTr("A 4K 60 USB card (MJPEG)"), qsTr("A PCIe card sending uncompressed video: nothing to decode")]] }
          ],
          more: [
              qsTr("Not suited to 4K at 60 with the AI filters: built-in graphics (such as the Radeon 780M) and entry laptop cards (RTX 3050 and 2050, MX). They are fine at 1080p 60, or 4K 30."),
              qsTr("Laptops vary a lot with their power limit: the same graphics chip can be much slower in a thin laptop than in a thick one, and slower again on battery."),
              qsTr("The figures above were measured with OmaBroadcast itself; other cards are estimated from them.")
          ] },

        { key: "faq-capture-60", kind: "faq", icon: "camera",
          name: qsTr("Why does my camera or capture card not reach 60 at 4K?"),
          summary: qsTr("At 4K and 60 frames a second most USB cards send MJPEG: every picture is a JPEG that the processor has to unpack. OmaBroadcast unpacks them on several cores at once; on an older processor, or with the processor busy, choose a smaller size."),
          also: "capture card camera mjpeg 4k 60 fps slow decode usb hdmi yuy2 nv12 uncompressed processor cpu",
          related: ["camera-mic", "dropped-frames", "faq-4k60-ai"],
          points: [
              qsTr("Choose the card's size and rate in its source settings. 1920 × 1080 at 60 is light for any computer."),
              qsTr("At 4K, 30 frames a second halves the work."),
              qsTr("A different encoder does not help here: the pictures arrive too slowly, before any encoder sees them.")
          ],
          tables: [
              { title: qsTr("What each kind of input costs"),
                columns: [qsTr("Input"), qsTr("How its pictures arrive"), qsTr("Processor")],
                rows: [[qsTr("Camera or card in MJPEG"), qsTr("A JPEG every frame, unpacked on several cores above 1080p"), qsTr("Highest")],
                       [qsTr("Camera or card uncompressed (YUY2, NV12)"), qsTr("Copied once; colour converted on the graphics card"), qsTr("Low")],
                       [qsTr("Display and window capture"), qsTr("Handed to the graphics card directly"), qsTr("Almost none")],
                       [qsTr("Media files and phones"), qsTr("Unpacked by the graphics card's video decoder"), qsTr("Low")],
                       [qsTr("Images"), qsTr("Unpacked once"), qsTr("None after loading")]] }
          ],
          more: [
              qsTr("Uncompressed 4K at 60 does not fit through USB 3, which is why cards compress it. A PCIe capture card can send it uncompressed, and then there is nothing to unpack."),
              qsTr("A media file recorded in MJPEG goes to whichever graphics card can unpack it: NVIDIA cards cannot take the 4:2:2 JPEG most cameras record, so another card is tried. With none, above 1080p it is unpacked on several cores.")
          ] },

        { key: "faq-judder", kind: "faq", icon: "activity",
          name: qsTr("Why does movement judder in my recording?"),
          summary: qsTr("Usually the source and the canvas run at different rates: a 50 Hz signal on a 60 fps canvas shows one picture in five twice, a hiccup ten times a second. Set the canvas to the source's rate."),
          also: "judder stutter jerky uneven frame rate 50 60 hz pal ntsc capture card cadence repeated frames smooth",
          related: ["faq-capture-60", "settings-video", "dropped-frames"],
          points: [
              qsTr("A capture card's settings say when its picture changes at a rate the canvas does not fit evenly, with a button that sets the canvas, and the card, to that rate."),
              qsTr("In the UK and Europe, televisions, consoles and cameras often run at 50 Hz; set the canvas to 50 (Settings ▸ Video ▸ Frame rate). Elsewhere it is usually 60."),
              qsTr("Rates that divide evenly are fine: a 30 fps camera on a 60 fps canvas shows every picture twice, smoothly.")
          ],
          more: [
              qsTr("Choosing the card's mode at the rate you want also sets the canvas to it."),
              qsTr("Judder is not dropped frames: the recording has every frame, some of them repeated. Dropped frames are counted in Stats.")
          ] },

        { key: "faq-encoder", kind: "faq", icon: "film",
          name: qsTr("Which encoder should I record with?"),
          summary: qsTr("H.264 or HEVC on the graphics card for nearly everything. APV only when you will edit the recording and the computer has graphics power to spare."),
          also: "encoder codec h264 h.264 hevc h.265 av1 apv nvenc vaapi which best record quality size choppy",
          related: ["settings-recording", "record-for-editing", "files-too-big"],
          points: [
              qsTr("H.264 and HEVC on the graphics card use a part of the chip of their own, so they hardly slow the drawing. HEVC makes smaller files at the same quality."),
              qsTr("AV1 makes files about as small as HEVC's, or a little smaller depending on the graphics card; some older players and editors cannot open it."),
              qsTr("APV is for editing: every frame stands alone and colours stay full. It runs on the graphics card's drawing cores, so at 4K it competes with the studio and its filters."),
              qsTr("Encoders on the processor are slower; use them only when the graphics card has none.")
          ],
          more: [
              qsTr("If frames are dropped whatever the encoder, the drawing is the limit, not the encoder: see Dropped frames, or What computer do I need."),
              qsTr("Best available picks for you from the encoders that really work on this computer, and passes over ones too slow for your canvas.")
          ] },

        { key: "faq-phone", kind: "faq", icon: "smartphone",
          name: qsTr("Can I use my iPhone or Android phone as a camera?"),
          summary: qsTr("Yes. Add Phone (network stream) and send to it from a streaming app on the phone, over Wi-Fi or a USB cable. Picture and sound arrive together."),
          also: "iphone android phone camera webcam moblin larix srt rtmp usb tether hotspot wireless continuity passphrase password",
          related: ["phone-camera", "sources"],
          points: [
              qsTr("On an iPhone the free app Moblin sends SRT and RTMP; Larix Broadcaster works too, with a subscription for some features. On Android, any app that sends SRT or RTMP."),
              qsTr("For the steadiest link, plug the phone in with Personal Hotspot on and send to the cable's address the source lists."),
              qsTr("The source shows a passphrase the phone must send, so that nobody else on the network can put a picture in your show."),
              qsTr("The walk-through Use your phone as a camera goes through it step by step.")
          ],
          more: [
              qsTr("An iPhone cannot be a plain USB webcam on Linux; the phone's own app is the way."),
              qsTr("SRT copes with lost packets; Latency sets how long it waits for them. RTMP suits apps that offer nothing else, and is fine over a cable.")
          ] },

        { key: "faq-green-screen", kind: "faq", icon: "scan",
          name: qsTr("Do I need a green screen?"),
          summary: qsTr("No. Remove background (AI) finds people in front of any background. A green screen with Chroma key still gives the cleanest edges, above all round hair, and costs the graphics card far less."),
          also: "green screen chroma key remove background ai person virtual background need",
          related: ["remove-background", "green-screen", "filters"],
          points: [
              qsTr("Remove background (AI) knows people only, not objects or pets."),
              qsTr("Chroma key keys anything in front of the screen, and is the lighter of the two."),
              qsTr("Either way, add Light wrap after it so you look lit by what is put behind you.")
          ] }
    ]
}
