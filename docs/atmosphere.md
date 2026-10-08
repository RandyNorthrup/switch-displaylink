# DisplayLink dock under Atmosphère (Horizon), in game

**Goal:** the same result as the L4T mirror: the game shows on the monitor plugged into
the Plugable UD-3900PDZ (DL-6950) dock, running Atmosphère on the Switch Lite.

Horizon has no DisplayLink driver, and DisplayLinkManager (DLM) can't run there.
So a sysmodule would have to do DLM's job:

1. **USB host to the dock** through Horizon's `usb:hs` (sys-con already drives USB
   devices this way). *Not checked yet:* does the Lite in Horizon enumerate the
   dock's hub and the DL-6950 (`17e9:4323`)?
2. **Game frames.** Capture is known to work: Switch Frame Tap reads the game's
   swapchain from a sysmodule.
3. **Speak the DL-6950 protocol:** authentication, firmware, mode set, and the
   compressed frame stream. This is the unknown part, and the work below is about it.

## Findings so far (2026-10-08, traffic of DLM 5.9.184 captured on L4T)

Captured with usbmon while dl-mirror was running (8 s steady state, `tcpdump -i usbmon1`):

- **Endpoints:** the frame data goes to **bulk OUT ep 0x02** (27.3 MB in 8 s:
  65536-byte URBs plus shorter ones). Control traffic in steady state is only
  standard GET_DESCRIPTOR requests.
- **The frame stream is not encrypted** (at least in steady state):
  - The overall randomness is 7.47 bits per byte (encrypted data is ≈ 8.0).
  - 47% of 16-byte blocks are exact repeats.
  - Two 20,944-byte URBs are byte-for-byte identical, which a cipher with a nonce or
    counter wouldn't produce.
  - Messages start with a readable header, for example
    `0000 4c0a 0400 0000 0000 0e00 0000 0000 1002 0128 0002 4001 …`, followed by what
    looks like a compressed bitstream.
- **The firmware packages are encrypted blobs** (`/opt/displaylink/*.spkg`, magic
  `ELLA`, randomness 7.9998 bits per byte), presumably decrypted by the chip. A
  sysmodule would upload the user's own copy unchanged; it can't be redistributed.
- **DLM authenticates the chip:** the binary contains
  `dl4nivo::NivoControl::AuthenticationException`,
  `dl4nivo::device::Head::AuthenticationException`, RSA, ECDH and HMAC-SHA256
  (mbedTLS-style), plus HDCP 2.2 code for HDCP monitors. So the dock attach includes
  a cryptographic handshake. **Replicating it is the main obstacle.**

## Next

1. Capture a full dock attach (handshake, firmware upload, mode set), then work out the
   control and bulk messages.
2. Find the handshake in DLM. The binary is aarch64 and stripped, but the C++ RTTI
   names survive (`dl4nivo::…`).
3. Check step 1 on Horizon: a small homebrew app that lists `usb:hs` devices with the
   dock attached.

Legal note: this is reverse engineering for interoperability with hardware randy owns.
No DisplayLink binaries, firmware or keys go in this public repo; anything like that is
read from the user's own DLM install at runtime.
