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

## Dock attach, captured (2026-10-08 00:21)

The capture: a clean eject (`dock-eject.sh`), then a physical replug, recorded with
`tcpdump -i usbmon1 -s 8192`. A raw pull doesn't work for this: the xHCI rebind
leaves tcpdump attached to a dead usbmon fd.

The endpoints DLM uses: **bulk OUT 0x02** (commands and frames) and **bulk IN 0x84**
(replies). The audio (iso 0x09) and Ethernet functions are handled by the kernel's
own drivers.

**Message framing** (both directions): `u16 0, u16 len-4, u16 channel, u16 flags
(0x8000 = reply), u16 type, ...`. Plaintext RPCs are type `0x25` (from the chip)
and `0x04` (from the host); the encrypted channel is type `0x24` (from the host)
and `0x45` (from the chip). Inside channel 4, the host's requests are service
`0x0010` and the chip's messages are `0x0084`, with tag `0x30`.

**Sequence:**
1. Vendor control requests: `c1 fe` (16 B IN), `40 24`, `c1 22` (28 B IN).
2. A plaintext session on channels 1, 2 and 4.
3. **HDCP 2.2 authentication, in the clear.** The message ids match the HDCP 2.2 spec:
   `02` AKE_Init (rtx), `13` AKE_Transmitter_Info, `03` AKE_Send_Cert (chip
   certificate), `14` AKE_Receiver_Info, `04` AKE_No_Stored_km (128 B, km encrypted
   to the chip's public key), `07` H′, `08` pairing info, `09` LC_Init, `0a` L′,
   `0b` SKE_Send_Eks, `0c` receiver ID list (the chip is a repeater), `0f` ack, and
   `10`/`11` stream manage/ready. All of it takes about 150 ms.
4. Then the **encrypted channel** (`0x24`/`0x45`, high-entropy payloads) for
   configuration.
5. Then the frame stream on ep 0x02 (not encrypted, see above).

**Why that matters:** an HDCP 2.2 *transmitter* holds no device secret. It picks km
and ks itself, encrypts km to the receiver's certificate key (RSA-1024 / OAEP), and
derives kd, H′, L′ and the rest with the published functions. So step 3 can be
reimplemented without any key from DLM. Checking the certificate against DCP's
public key is optional for us.

**Still unknown:** the key and cipher of the `0x24`/`0x45` channel (probably derived
from the HDCP session, but not confirmed), what the configuration messages mean
(including the mode set), the frame command format, and the compression.

## Next

1. ~~Capture a full dock attach~~ (done, above).
2. Find out how DLM keys the `0x24`/`0x45` channel. The binary is aarch64 and
   stripped, but the C++ RTTI names survive (`dl4nivo::…`).
3. Find out what the type `0x08` messages on channel 4 are. They're high-entropy, 11
   messages totaling ~391 KB, sent 0.5 s and 12.4-12.9 s after attach, around when
   the display started. None of their bytes match the `.spkg` files directly, so if
   they're a firmware upload, it's re-wrapped in the session encryption. Unknown for
   now. Compare with a cold dock (dock unpowered first).
   The plain frame data is type `0x00` on channel 4 (`0000 xxxx 0400 0000 0000 …`).
4. Check step 1 on Horizon: a small homebrew app that lists `usb:hs` devices with the
   dock attached.

Legal note: this is reverse engineering for interoperability with hardware randy owns.
No DisplayLink binaries, firmware or keys go in this public repo; anything like that is
read from the user's own DLM install at runtime.
