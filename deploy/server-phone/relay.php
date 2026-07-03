<?php
declare(strict_types=1);

header('Cache-Control: no-store, no-cache, must-revalidate');

const HFS_API_KEY = 'vnc-hfs-20260629';
const HFS_RELAY_REQUEST_TTL_SECONDS = 300;
const HFS_RELAY_RESPONSE_KEEP_SECONDS = 120;

function json_response(int $statusCode, array $payload): never
{
    http_response_code($statusCode);
    header('Content-Type: application/json; charset=utf-8');
    echo json_encode($payload, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    exit;
}

function data_root(): string
{
    return __DIR__ . DIRECTORY_SEPARATOR . 'data';
}

function relay_root(): string
{
    return data_root() . DIRECTORY_SEPARATOR . 'relay';
}

function safe_token(string $value): string
{
    return preg_replace('/[^a-zA-Z0-9._-]+/', '', $value) ?? '';
}

function instance_dir(string $instanceId): string
{
    return relay_root() . DIRECTORY_SEPARATOR . safe_token($instanceId);
}

function heartbeat_path(string $instanceId): string
{
    return instance_dir($instanceId) . DIRECTORY_SEPARATOR . 'heartbeat.txt';
}

function touch_instance_heartbeat(string $instanceId): void
{
    if ($instanceId === '') {
        return;
    }

    if (!ensure_instance_dirs($instanceId)) {
        return;
    }

    @file_put_contents(heartbeat_path($instanceId), gmdate('c'), LOCK_EX);
}

function requests_dir(string $instanceId): string
{
    return instance_dir($instanceId) . DIRECTORY_SEPARATOR . 'requests';
}

function responses_dir(string $instanceId): string
{
    return instance_dir($instanceId) . DIRECTORY_SEPARATOR . 'responses';
}

function request_meta_path(string $instanceId, string $requestId): string
{
    return requests_dir($instanceId) . DIRECTORY_SEPARATOR . safe_token($requestId) . '.json';
}

function request_body_path(string $instanceId, string $requestId): string
{
    return requests_dir($instanceId) . DIRECTORY_SEPARATOR . safe_token($requestId) . '.body';
}

function response_meta_path(string $instanceId, string $requestId): string
{
    return responses_dir($instanceId) . DIRECTORY_SEPARATOR . safe_token($requestId) . '.json';
}

function response_body_path(string $instanceId, string $requestId): string
{
    return responses_dir($instanceId) . DIRECTORY_SEPARATOR . safe_token($requestId) . '.body';
}

function response_done_path(string $instanceId, string $requestId): string
{
    return responses_dir($instanceId) . DIRECTORY_SEPARATOR . safe_token($requestId) . '.done';
}

function response_error_path(string $instanceId, string $requestId): string
{
    return responses_dir($instanceId) . DIRECTORY_SEPARATOR . safe_token($requestId) . '.error.json';
}

function ensure_instance_dirs(string $instanceId): bool
{
    $directories = [
        data_root(),
        relay_root(),
        instance_dir($instanceId),
        requests_dir($instanceId),
        responses_dir($instanceId),
    ];

    foreach ($directories as $directory) {
        if (!is_dir($directory) && !mkdir($directory, 0777, true) && !is_dir($directory)) {
            return false;
        }
    }

    return true;
}

function load_json_file(string $path): ?array
{
    if (!is_file($path)) {
        return null;
    }

    $decoded = json_decode(file_get_contents($path) ?: '', true);
    return is_array($decoded) ? $decoded : null;
}

function save_json_file(string $path, array $payload): bool
{
    $json = json_encode($payload, JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    if ($json === false) {
        return false;
    }

    return file_put_contents($path, $json, LOCK_EX) !== false;
}

function delete_request_bundle(string $instanceId, string $requestId): void
{
    @unlink(request_meta_path($instanceId, $requestId));
    @unlink(request_body_path($instanceId, $requestId));
    @unlink(response_meta_path($instanceId, $requestId));
    @unlink(response_body_path($instanceId, $requestId));
    @unlink(response_done_path($instanceId, $requestId));
    @unlink(response_error_path($instanceId, $requestId));
}

function cleanup_instance(string $instanceId): void
{
    $requestFiles = glob(requests_dir($instanceId) . DIRECTORY_SEPARATOR . '*.json') ?: [];
    $now = time();

    foreach ($requestFiles as $metaPath) {
        $requestId = pathinfo($metaPath, PATHINFO_FILENAME);
        $meta = load_json_file($metaPath);
        $createdAt = strtotime((string)($meta['createdAt'] ?? ''));
        $doneMtime = @filemtime(response_done_path($instanceId, $requestId));
        $errorMtime = @filemtime(response_error_path($instanceId, $requestId));

        $expired = $createdAt === false || ($now - $createdAt) > HFS_RELAY_REQUEST_TTL_SECONDS;
        $completedOld = ($doneMtime !== false && ($now - $doneMtime) > HFS_RELAY_RESPONSE_KEEP_SECONDS)
                        || ($errorMtime !== false && ($now - $errorMtime) > HFS_RELAY_RESPONSE_KEEP_SECONDS);

        if ($expired || $completedOld) {
            delete_request_bundle($instanceId, $requestId);
        }
    }
}

function claim_pending_request(string $instanceId): ?array
{
    if (!ensure_instance_dirs($instanceId)) {
        return null;
    }

    $lockHandle = fopen(instance_dir($instanceId) . DIRECTORY_SEPARATOR . 'pull.lock', 'c+');
    if ($lockHandle === false) {
        return null;
    }

    if (!flock($lockHandle, LOCK_EX)) {
        fclose($lockHandle);
        return null;
    }

    cleanup_instance($instanceId);

    $requestFiles = glob(requests_dir($instanceId) . DIRECTORY_SEPARATOR . '*.json') ?: [];
    usort($requestFiles, static function (string $left, string $right): int {
        return (filemtime($left) ?: 0) <=> (filemtime($right) ?: 0);
    });

    $now = time();
    $claimed = null;

    foreach ($requestFiles as $metaPath) {
        $meta = load_json_file($metaPath);
        if (!is_array($meta)) {
            $requestId = pathinfo($metaPath, PATHINFO_FILENAME);
            delete_request_bundle($instanceId, $requestId);
            continue;
        }

        $requestId = safe_token((string)($meta['id'] ?? pathinfo($metaPath, PATHINFO_FILENAME)));
        if ($requestId === '') {
            continue;
        }

        if (is_file(response_meta_path($instanceId, $requestId)) || is_file(response_error_path($instanceId, $requestId))) {
            continue;
        }

        $state = (string)($meta['state'] ?? 'pending');
        $assignedAt = strtotime((string)($meta['assignedAt'] ?? ''));
        $claimable = $state === 'pending'
                     || ($state === 'assigned' && ($assignedAt === false || ($now - $assignedAt) > 20));

        if (!$claimable) {
            continue;
        }

        $meta['id'] = $requestId;
        $meta['state'] = 'assigned';
        $meta['assignedAt'] = gmdate('c');
        if (!save_json_file($metaPath, $meta)) {
            continue;
        }

        $claimed = $meta;
        break;
    }

    flock($lockHandle, LOCK_UN);
    fclose($lockHandle);
    return $claimed;
}

function require_api_key(): void
{
    if (($_SERVER['HTTP_X_HFS_KEY'] ?? '') !== HFS_API_KEY) {
        json_response(403, ['ok' => false, 'error' => 'forbidden']);
    }
}

function read_json_payload(): array
{
    $decoded = json_decode(file_get_contents('php://input') ?: '', true);
    if (!is_array($decoded)) {
        json_response(400, ['ok' => false, 'error' => 'invalid_json']);
    }
    return $decoded;
}

function normalized_headers_array($headers): array
{
    if (!is_array($headers)) {
        return [];
    }

    $normalized = [];
    foreach ($headers as $name => $value) {
        if (!is_string($name) || trim($name) === '') {
            continue;
        }

        if (is_array($value)) {
            $values = [];
            foreach ($value as $single) {
                if (is_scalar($single)) {
                    $values[] = (string)$single;
                }
            }
            if (!empty($values)) {
                $normalized[$name] = $values;
            }
            continue;
        }

        if (is_scalar($value)) {
            $normalized[$name] = [(string)$value];
        }
    }

    return $normalized;
}

function stream_input_to_file(string $path): int
{
    $input = fopen('php://input', 'rb');
    $output = fopen($path, 'wb');
    if ($input === false || $output === false) {
        if (is_resource($input)) {
            fclose($input);
        }
        if (is_resource($output)) {
            fclose($output);
        }
        return -1;
    }

    $written = 0;
    while (!feof($input)) {
        $chunk = fread($input, 65536);
        if ($chunk === false) {
            fclose($input);
            fclose($output);
            return -1;
        }

        if ($chunk === '') {
            usleep(20000);
            continue;
        }

        $count = fwrite($output, $chunk);
        if ($count === false) {
            fclose($input);
            fclose($output);
            return -1;
        }

        $written += $count;
    }

    fflush($output);
    fclose($input);
    fclose($output);
    return $written;
}

require_api_key();

$action = trim((string)($_GET['action'] ?? $_POST['action'] ?? ''));
$instanceId = safe_token((string)($_GET['instanceId'] ?? $_POST['instanceId'] ?? ''));
$requestId = safe_token((string)($_GET['requestId'] ?? $_POST['requestId'] ?? ''));

if ($instanceId === '') {
    if (!in_array($action, ['begin-response', 'fail-response'], true)) {
        json_response(400, ['ok' => false, 'error' => 'instance_required']);
    }
}

switch ($action) {
    case 'pull': {
        $waitSeconds = (int)($_GET['wait'] ?? $_POST['wait'] ?? 15);
        $waitSeconds = max(1, min(25, $waitSeconds));

        if (!ensure_instance_dirs($instanceId)) {
            json_response(500, ['ok' => false, 'error' => 'create_instance_failed']);
        }

        touch_instance_heartbeat($instanceId);

        $deadline = microtime(true) + $waitSeconds;
        do {
            $request = claim_pending_request($instanceId);
            if (is_array($request)) {
                json_response(200, [
                    'ok' => true,
                    'found' => true,
                    'request' => [
                        'id' => (string)($request['id'] ?? ''),
                        'method' => (string)($request['method'] ?? 'GET'),
                        'path' => (string)($request['path'] ?? '/'),
                        'query' => (string)($request['query'] ?? ''),
                        'headers' => $request['headers'] ?? new stdClass(),
                        'hasBody' => (bool)($request['hasBody'] ?? false),
                        'bodySize' => (int)($request['bodySize'] ?? 0),
                    ],
                ]);
            }

            usleep(250000);
        } while (microtime(true) < $deadline);

        json_response(200, ['ok' => true, 'found' => false]);
    }

    case 'request-body': {
        if ($requestId === '') {
            json_response(400, ['ok' => false, 'error' => 'request_required']);
        }

        $path = request_body_path($instanceId, $requestId);
        if (!is_file($path)) {
            http_response_code(204);
            exit;
        }

        header('Content-Type: application/octet-stream');
        header('Content-Length: ' . (string)(filesize($path) ?: 0));
        readfile($path);
        exit;
    }

    case 'begin-response': {
        $payload = read_json_payload();
        $instanceId = safe_token((string)($payload['instanceId'] ?? $instanceId));
        $requestId = safe_token((string)($payload['requestId'] ?? $requestId));
        if ($instanceId === '' || $requestId === '') {
            json_response(400, ['ok' => false, 'error' => 'request_required']);
        }

        if (!ensure_instance_dirs($instanceId)) {
            json_response(500, ['ok' => false, 'error' => 'create_instance_failed']);
        }

        touch_instance_heartbeat($instanceId);

        $meta = [
            'requestId' => $requestId,
            'statusCode' => max(100, min(599, (int)($payload['statusCode'] ?? 502))),
            'headers' => normalized_headers_array($payload['headers'] ?? []),
            'isHtml' => (bool)($payload['isHtml'] ?? false),
            'updatedAt' => gmdate('c'),
        ];

        @unlink(response_done_path($instanceId, $requestId));
        @unlink(response_error_path($instanceId, $requestId));
        @unlink(response_body_path($instanceId, $requestId));

        if (!save_json_file(response_meta_path($instanceId, $requestId), $meta)) {
            json_response(500, ['ok' => false, 'error' => 'write_failed']);
        }

        json_response(200, ['ok' => true]);
    }

    case 'upload-response': {
        if ($requestId === '') {
            json_response(400, ['ok' => false, 'error' => 'request_required']);
        }
        if (!ensure_instance_dirs($instanceId)) {
            json_response(500, ['ok' => false, 'error' => 'create_instance_failed']);
        }
        if (!is_file(response_meta_path($instanceId, $requestId))) {
            json_response(404, ['ok' => false, 'error' => 'response_not_initialized']);
        }

        touch_instance_heartbeat($instanceId);

        $bytes = stream_input_to_file(response_body_path($instanceId, $requestId));
        if ($bytes < 0) {
            json_response(500, ['ok' => false, 'error' => 'write_failed']);
        }

        file_put_contents(response_done_path($instanceId, $requestId), (string)$bytes, LOCK_EX);
        json_response(200, ['ok' => true, 'bytes' => $bytes]);
    }

    case 'fail-response': {
        $payload = read_json_payload();
        $instanceId = safe_token((string)($payload['instanceId'] ?? $instanceId));
        $requestId = safe_token((string)($payload['requestId'] ?? $requestId));
        if ($instanceId === '' || $requestId === '') {
            json_response(400, ['ok' => false, 'error' => 'request_required']);
        }

        if (!ensure_instance_dirs($instanceId)) {
            json_response(500, ['ok' => false, 'error' => 'create_instance_failed']);
        }

        touch_instance_heartbeat($instanceId);

        $errorPayload = [
            'requestId' => $requestId,
            'message' => trim((string)($payload['message'] ?? 'Android relay failed')),
            'updatedAt' => gmdate('c'),
        ];

        if (!save_json_file(response_error_path($instanceId, $requestId), $errorPayload)) {
            json_response(500, ['ok' => false, 'error' => 'write_failed']);
        }

        json_response(200, ['ok' => true]);
    }
}

json_response(400, ['ok' => false, 'error' => 'unknown_action']);
