<?php
$http = new Swoole\Http\Server("0.0.0.0", 9501, SWOOLE_BASE);
//$http = new swoole_http_server("0.0.0.0", 9501);
$http->set([
    'enable_static_handler' => true,
    'document_root' => __DIR__,
]);

$http->on('request', function ($req, $resp) {
    $resp->end("hello world\n");
});

$http->start();
