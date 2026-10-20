# Sourced by startplasma (/etc/xdg/plasma-workspace/env/) before KWin starts.
# With the DisplayLink dock attached and a saved layout, extend the desktop
# onto its monitors (unless "dl-extend mirror" was chosen) and let KWin/Qt see
# them.
if /usr/local/bin/dl-extend session; then
	export LD_PRELOAD="/usr/local/lib/dl-fakerandr.so${LD_PRELOAD:+:$LD_PRELOAD}"
fi
