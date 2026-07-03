<?php
declare(strict_types=1);

if (function_exists('apache_setenv')) {
    @apache_setenv('no-gzip', '1');
}
@ini_set('output_buffering', '0');
@ini_set('zlib.output_compression', '0');
@ini_set('implicit_flush', '1');
while (ob_get_level() > 0) {
    @ob_end_flush();
}
ob_implicit_flush(1);
ignore_user_abort(true);
set_time_limit(0);

header('X-Accel-Buffering: no');
header('Cache-Control: no-store, no-cache, must-revalidate');

const HFS_ALIAS_TTL_SECONDS = 300;

function u(string $value): string
{
    $decoded = json_decode('"' . $value . '"');
    return is_string($decoded) ? $decoded : $value;
}

function load_aliases(): array
{
    $baseDir = defined('HFS_STORE_DIR') && is_string(HFS_STORE_DIR) && HFS_STORE_DIR !== ''
        ? HFS_STORE_DIR
        : __DIR__ . DIRECTORY_SEPARATOR . 'data';
    $path = $baseDir . DIRECTORY_SEPARATOR . 'aliases.json';
    if (!is_file($path)) {
        return [];
    }

    $json = file_get_contents($path);
    $decoded = json_decode($json ?: '', true);
    if (!is_array($decoded) || !is_array($decoded['aliases'] ?? null)) {
        return [];
    }

    $now = time();
    $aliases = $decoded['aliases'];
    foreach ($aliases as $key => $entry) {
        if (!is_array($entry)) {
            unset($aliases[$key]);
            continue;
        }

        $updatedAt = strtotime((string)($entry['updatedAt'] ?? ''));
        if ($updatedAt === false || ($now - $updatedAt) > HFS_ALIAS_TTL_SECONDS) {
            unset($aliases[$key]);
        }
    }

    return $aliases;
}

function forward_cookie_header(string $cookieHeader): string
{
    $pairs = array_map('trim', explode(';', $cookieHeader));
    $allowed = [];
    foreach ($pairs as $pair) {
        if ($pair === '') {
            continue;
        }

        if (stripos($pair, 'hfs_auth=') === 0 || stripos($pair, 'hfs_') === 0) {
            $allowed[] = $pair;
        }
    }

    return implode('; ', $allowed);
}

function alias_base(string $alias): string
{
    $basePath = defined('HFS_PUBLIC_BASE_PATH') && is_string(HFS_PUBLIC_BASE_PATH) && HFS_PUBLIC_BASE_PATH !== ''
        ? rtrim(HFS_PUBLIC_BASE_PATH, '/')
        : '/HFS';
    return $basePath . '/' . rawurlencode($alias);
}

function starts_with(string $value, string $prefix): bool
{
    return $prefix === '' || strpos($value, $prefix) === 0;
}

function rebuild_path(string $path, string $query = '', string $fragment = ''): string
{
    return $path
        . ($query !== '' ? '?' . $query : '')
        . ($fragment !== '' ? '#' . $fragment : '');
}

function rewrite_public_path(string $path, string $alias, string $shareRoute): string
{
    if ($path === '' || starts_with($path, '//')) {
        return $path;
    }

    $parts = parse_url($path);
    if ($parts === false) {
        return $path;
    }

    $rawPath = (string)($parts['path'] ?? '');
    $query = (string)($parts['query'] ?? '');
    $fragment = (string)($parts['fragment'] ?? '');
    $aliasBase = alias_base($alias);

    if ($rawPath === '' || $rawPath === '/') {
        return rebuild_path($aliasBase, $query, $fragment);
    }

    if ($rawPath === $aliasBase || starts_with($rawPath, $aliasBase . '/')) {
        return rebuild_path($rawPath, $query, $fragment);
    }

    foreach (['/__auth', '/__upload', '/__logo'] as $special) {
        if ($rawPath === $special || starts_with($rawPath, $special . '/')) {
            return rebuild_path($aliasBase . $rawPath, $query, $fragment);
        }
    }

    $shareRoute = trim($shareRoute, '/');
    if ($shareRoute === '') {
        return rebuild_path($aliasBase . $rawPath, $query, $fragment);
    }

    $routePrefix = '/' . $shareRoute;
    if ($rawPath === $routePrefix) {
        return rebuild_path($aliasBase, $query, $fragment);
    }

    if (starts_with($rawPath, $routePrefix . '/')) {
        return rebuild_path($aliasBase . substr($rawPath, strlen($routePrefix)), $query, $fragment);
    }

    return $path;
}

function rewrite_location(string $location, string $alias, string $shareRoute, string $targetBase): string
{
    $targetBase = rtrim($targetBase, '/');

    if ($targetBase !== '' && starts_with($location, $targetBase)) {
        $suffix = substr($location, strlen($targetBase));
        return rewrite_public_path($suffix === '' ? '/' : $suffix, $alias, $shareRoute);
    }

    if (starts_with($location, '/')) {
        return rewrite_public_path($location, $alias, $shareRoute);
    }

    return $location;
}

function rewrite_html(string $html, string $alias, string $shareRoute, string $targetBase): string
{
    $rewritten = preg_replace_callback(
        '/([\'"])(\/[^\'"]*)\1/',
        static function (array $matches) use ($alias, $shareRoute): string {
            return $matches[1] . rewrite_public_path($matches[2], $alias, $shareRoute) . $matches[1];
        },
        $html
    );

    $html = is_string($rewritten) ? $rewritten : $html;
    $targetBase = rtrim($targetBase, '/');

    $rewritten = preg_replace_callback(
        '/([\'"])(https?:\/\/[^\'"]+)\1/i',
        static function (array $matches) use ($alias, $shareRoute, $targetBase): string {
            $url = $matches[2];
            if ($targetBase !== '' && starts_with($url, $targetBase)) {
                $suffix = substr($url, strlen($targetBase));
                $url = rewrite_public_path($suffix === '' ? '/' : $suffix, $alias, $shareRoute);
            }
            return $matches[1] . $url . $matches[1];
        },
        $html
    );

    return is_string($rewritten) ? $rewritten : $html;
}

function fail_page(int $statusCode, string $title, string $message): never
{
    http_response_code($statusCode);
    header('Content-Type: text/html; charset=utf-8');
    echo '<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">';
    echo '<title>' . htmlspecialchars($title, ENT_QUOTES, 'UTF-8') . '</title>';
    echo '<style>body{font-family:"Microsoft JhengHei UI",sans-serif;background:#eef5ff;color:#163152;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}.box{width:min(560px,92vw);background:#fff;border-radius:24px;padding:30px;box-shadow:0 16px 40px rgba(27,70,139,.12)}h1{margin:0 0 10px;font-size:28px}p{color:#6d83a6;font-size:16px;line-height:1.7}</style></head><body><div class="box">';
    echo '<h1>' . htmlspecialchars($title, ENT_QUOTES, 'UTF-8') . '</h1>';
    echo '<p>' . htmlspecialchars($message, ENT_QUOTES, 'UTF-8') . '</p>';
    echo '</div></body></html>';
    exit;
}

function encode_segments(array $segments): string
{
    return implode('/', array_map(
        static function (string $part): string {
            return rawurlencode(rawurldecode($part));
        },
        $segments
    ));
}

$aliases = load_aliases();
$basePath = defined('HFS_PUBLIC_BASE_PATH') && is_string(HFS_PUBLIC_BASE_PATH) && HFS_PUBLIC_BASE_PATH !== ''
    ? rtrim(HFS_PUBLIC_BASE_PATH, '/')
    : '/HFS';
$requestPath = parse_url($_SERVER['REQUEST_URI'] ?? $basePath, PHP_URL_PATH) ?: $basePath;
$relative = trim(substr($requestPath, strlen($basePath)), '/');

if ($relative === '' || $relative === 'index.php') {
    fail_page(200, 'Easy Cloud HFS', u('\u8acb\u8f38\u5165\u6b63\u78ba\u7684\u5206\u4eab\u9023\u7d50\u5f8c\u518d\u958b\u555f\u3002'));
}

$segments = array_values(array_filter(explode('/', $relative), static function ($part) {
    return $part !== '';
}));
$alias = rawurldecode((string)array_shift($segments));
$config = $aliases[$alias] ?? null;
if (!is_array($config)) {
    fail_page(404, u('\u627e\u4e0d\u5230\u5206\u4eab'), u('\u9019\u500b\u5916\u90e8\u5206\u4eab\u9023\u7d50\u76ee\u524d\u4e0d\u5b58\u5728\u6216\u5df2\u505c\u7528\u3002'));
}

$targetBase = rtrim((string)($config['targetBase'] ?? ''), '/');
$shareRoute = trim((string)($config['shareRoute'] ?? ''), '/');
if ($targetBase === '') {
    fail_page(500, u('\u8a2d\u5b9a\u932f\u8aa4'), u('\u9019\u500b\u5206\u4eab\u5c1a\u672a\u5b8c\u6210\u5916\u90e8\u9023\u7d50\u8a2d\u5b9a\u3002'));
}

$special = $segments[0] ?? '';
if (in_array($special, ['__auth', '__upload', '__logo'], true)) {
    array_shift($segments);
    $targetPath = '/' . $special;
    if (!empty($segments)) {
        $targetPath .= '/' . encode_segments($segments);
    }
} else {
    $targetPath = $shareRoute === '' ? '/' : '/' . rawurlencode($shareRoute);
    if (!empty($segments)) {
        $suffix = encode_segments($segments);
        $targetPath .= ($targetPath === '/' ? '' : '/') . $suffix;
    }
}

$query = $_SERVER['QUERY_STRING'] ?? '';
$targetUrl = $targetBase . $targetPath . ($query !== '' ? '?' . $query : '');
$method = $_SERVER['REQUEST_METHOD'] ?? 'GET';
$body = file_get_contents('php://input') ?: '';
$contentType = $_SERVER['CONTENT_TYPE'] ?? '';

$curl = curl_init($targetUrl);
if ($curl === false) {
    fail_page(500, u('\u4ee3\u7406\u932f\u8aa4'), u('\u76ee\u524d\u7121\u6cd5\u5efa\u7acb\u5916\u90e8\u4ee3\u7406\u9023\u7dda\u3002'));
}

$forwardHeaders = [];
if (!empty($_SERVER['HTTP_RANGE'])) {
    $forwardHeaders[] = 'Range: ' . $_SERVER['HTTP_RANGE'];
}
if (!empty($_SERVER['HTTP_ACCEPT'])) {
    $forwardHeaders[] = 'Accept: ' . $_SERVER['HTTP_ACCEPT'];
}
if (!empty($_SERVER['HTTP_ACCEPT_LANGUAGE'])) {
    $forwardHeaders[] = 'Accept-Language: ' . $_SERVER['HTTP_ACCEPT_LANGUAGE'];
}
if (!empty($_SERVER['HTTP_USER_AGENT'])) {
    $forwardHeaders[] = 'User-Agent: ' . $_SERVER['HTTP_USER_AGENT'];
}
if (!empty($_SERVER['HTTP_COOKIE'])) {
    $cookieHeader = forward_cookie_header((string)$_SERVER['HTTP_COOKIE']);
    if ($cookieHeader !== '') {
        $forwardHeaders[] = 'Cookie: ' . $cookieHeader;
    }
}

$clientIp = trim((string)($_SERVER['HTTP_CF_CONNECTING_IP'] ?? $_SERVER['HTTP_X_FORWARDED_FOR'] ?? $_SERVER['REMOTE_ADDR'] ?? ''));
if ($clientIp !== '') {
    $forwardHeaders[] = 'X-Forwarded-For: ' . $clientIp;
}

if ($contentType !== '') {
    $forwardHeaders[] = 'Content-Type: ' . $contentType;
}
if (in_array($method, ['POST', 'PUT', 'PATCH'], true)) {
    $forwardHeaders[] = 'Expect:';
}

$statusCode = 502;
$isHtml = false;
$bufferResponseBody = false;
$responseBody = '';

curl_setopt_array($curl, [
    CURLOPT_CUSTOMREQUEST => $method,
    CURLOPT_FOLLOWLOCATION => false,
    CURLOPT_RETURNTRANSFER => false,
    CURLOPT_HEADER => false,
    CURLOPT_HTTPHEADER => $forwardHeaders,
    CURLOPT_HTTP_VERSION => CURL_HTTP_VERSION_1_1,
    CURLOPT_POSTFIELDS => in_array($method, ['POST', 'PUT', 'PATCH'], true) ? $body : null,
    CURLOPT_NOBODY => $method === 'HEAD',
    CURLOPT_CONNECTTIMEOUT => 10,
    CURLOPT_TIMEOUT => 0,
    CURLOPT_BUFFERSIZE => 65536,
    CURLOPT_TCP_KEEPALIVE => 1,
    CURLOPT_HEADERFUNCTION => static function ($ch, string $header) use (&$statusCode, &$isHtml, &$bufferResponseBody, $alias, $shareRoute, $targetBase): int {
        $trimmed = trim($header);
        if ($trimmed === '') {
            return strlen($header);
        }

        if (preg_match('#^HTTP/\d+(?:\.\d+)?\s+(\d+)#', $trimmed, $match)) {
            $statusCode = (int)$match[1];
            if ($statusCode >= 400) {
                $bufferResponseBody = true;
            }
            http_response_code($statusCode);
            return strlen($header);
        }

        $colon = strpos($trimmed, ':');
        if ($colon === false) {
            return strlen($header);
        }

        $name = substr($trimmed, 0, $colon);
        $value = trim(substr($trimmed, $colon + 1));
        $lower = strtolower($name);

        if ($lower === 'content-type' && stripos($value, 'text/html') !== false) {
            $isHtml = true;
            $bufferResponseBody = true;
        }

        if (in_array($lower, ['content-length', 'transfer-encoding', 'connection', 'content-encoding'], true)) {
            return strlen($header);
        }

        if ($lower === 'location') {
            $value = rewrite_location($value, $alias, $shareRoute, $targetBase);
        }

        header($name . ': ' . $value, false);
        return strlen($header);
    },
    CURLOPT_WRITEFUNCTION => static function ($ch, string $chunk) use (&$isHtml, &$bufferResponseBody, &$responseBody): int {
        if ($isHtml || $bufferResponseBody) {
            $responseBody .= $chunk;
        } else {
            echo $chunk;
            flush();
        }
        return strlen($chunk);
    },
]);

$ok = curl_exec($curl);
$error = curl_error($curl);
curl_close($curl);

if ($ok === false) {
    fail_page(502, u('\u4ee3\u7406\u932f\u8aa4'), $error !== '' ? $error : u('\u76ee\u524d\u7121\u6cd5\u9023\u5230\u5916\u90e8\u5206\u4eab\u4f86\u6e90\u3002'));
}

if (!$isHtml && $bufferResponseBody) {
    $trimmedBody = trim($responseBody);
    if ($statusCode === 530
        || stripos($trimmedBody, 'origin has been unregistered from argo tunnel') !== false
        || stripos($trimmedBody, 'error code: 1000') !== false) {
        fail_page(502,
                  u('\u5916\u90e8\u9023\u7d50\u66ab\u6642\u7121\u6cd5\u4f7f\u7528'),
                  u('\u76ee\u524d\u7121\u6cd5\u9023\u5230\u9019\u500b\u5916\u90e8\u5206\u4eab\u3002\u8acb\u7a0d\u5f8c\u518d\u8a66\uff0c\u6216\u56de\u5230\u684c\u9762\u7a0b\u5f0f\u91cd\u65b0\u555f\u52d5\u4f3a\u670d\u5668\u8207\u5916\u90e8\u9023\u7d50\u3002'));
    }
}

if ($isHtml) {
    echo rewrite_html($responseBody, $alias, $shareRoute, $targetBase);
} else if ($bufferResponseBody) {
    echo $responseBody;
} else {
    echo $responseBody;
}
