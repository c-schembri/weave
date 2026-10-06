# TLS Echo

`server.cpp` upgrades accepted TCP streams to TLS before echoing bytes. Clients
run concurrently through `tcp::serve`; each owns its encrypted stream and buffer.
The handler exchanges `close_notify` on EOF rather than treating socket closure
as a successful TLS shutdown.

Build the optional TLS preset with OpenSSL 3 installed:

```sh
cmake --preset windows-tls -DOPENSSL_ROOT_DIR=C:/Libraries/openssl
cmake --build --preset tls-release --parallel 4
./build/windows-tls/Release/tls_echo_server.exe certificate.pem key.pem 8443
```

Supply your own PEM certificate chain and matching private key. No certificates,
private keys, or insecure verification bypasses are shipped with this example.
Use a trusted issuer or configure the client with the issuer's CA file.

The example uses one calling thread. Replacing Context setup with Runtime setup
does not change the TLS handler. See [TLS contracts](../../../../docs/tls.md).
