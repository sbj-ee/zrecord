Fixtures made with the real `minisign` 0.12 and a throwaway **test** key
(its secret key was deleted; it signs nothing else):

    minisign -G -W -p test.pub -s test.key
    minisign -S -s test.key -m SHA256SUMS -t "timestamp:0 file:SHA256SUMS hashed"
    minisign -S -l -s test.key -m SHA256SUMS.legacy -t "legacy test"   # "Ed" (not prehashed)

They prove zrecord's verifier reads minisign's own output. Not the release key.

`release-key/` is the public `SHA256SUMS` and `SHA256SUMS.minisig` of zterminal's v1.6.0 release,
signed with the real release key (a public file, copied unchanged from that release; the
signature covers those exact bytes, so the zterminal file names stay). It proves the verifier
reads real minisign output; the test pins zterminal's public key (`32960BBA77B36F1F`) as a constant.
The shared release key (`DBC46B23C3AF0EF3`, also zwriter's and zedit's) is the one compiled into `src/UpdateSigningKey.cpp`.
