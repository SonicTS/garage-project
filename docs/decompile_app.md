In order to make an mitm attack on an android app, the app needs to trust the certificate of the mitmproxy.
Most apps only allow their own/system certificates and dont allow user certificates. This can be deactivated by decompiling the app, 
and rebuilding with some settings in the AndroidManifest.xml