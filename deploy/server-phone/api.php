<?php
declare(strict_types=1);

header('Content-Type: application/json; charset=utf-8');

const HFS_API_KEY = 'vnc-hfs-20260629';
const HFS_ALIAS_TTL_SECONDS = 86400;
const HFS_INSTANCE_HEARTBEAT_TTL_SECONDS = 180;

function slugify_alias(string $value): string
{
    $value = trim(mb_strtolower($value, 'UTF-8'));
    $value = preg_replace('/[^a-z0-9]+/u', '-', $value) ?? '';
    $value = trim($value, '-');
    return $value;
}

function aliases_path(): string
{
    return __DIR__ . DIRECTORY_SEPARATOR . 'data' . DIRECTORY_SEPARATOR . 'aliases.json';
}

function relay_root(): string
{
    return __DIR__ . DIRECTORY_SEPARATOR . 'data' . DIRECTORY_SEPARATOR . 'relay';
}

function safe_token(string $value): string
{
    return preg_replace('/[^a-zA-Z0-9._-]+/', '', $value) ?? '';
}

function heartbeat_path(string $instanceId): string
{
    return relay_root() . DIRECTORY_SEPARATOR . safe_token($instanceId) . DIRECTORY_SEPARATOR . 'heartbeat.txt';
}

function instance_is_alive(string $instanceId): bool
{
    $instanceId = safe_token($instanceId);
    if ($instanceId === '') {
        return false;
    }

    $path = heartbeat_path($instanceId);
    if (!is_file($path)) {
        return false;
    }

    $mtime = @filemtime($path);
    if ($mtime === false) {
        return false;
    }

    return (time() - $mtime) <= HFS_INSTANCE_HEARTBEAT_TTL_SECONDS;
}

function purge_expired_aliases(array $aliases): array
{
    $now = time();
    foreach ($aliases as $key => $entry) {
        if (!is_array($entry)) {
            unset($aliases[$key]);
            continue;
        }

        $updatedAt = strtotime((string)($entry['updatedAt'] ?? ''));
        $instanceId = safe_token((string)($entry['instanceId'] ?? ''));
        $relayMode = trim((string)($entry['relayMode'] ?? ''));
        $ownerStale = $relayMode === 'android'
                      && $instanceId !== ''
                      && !instance_is_alive($instanceId);

        if ($updatedAt === false || ($now - $updatedAt) > HFS_ALIAS_TTL_SECONDS || $ownerStale) {
            unset($aliases[$key]);
        }
    }

    return $aliases;
}

function load_alias_store(): array
{
    $path = aliases_path();
    if (!is_file($path)) {
        return [
            'updatedAt' => gmdate('c'),
            'aliases' => [],
        ];
    }

    $decoded = json_decode(file_get_contents($path) ?: '', true);
    if (!is_array($decoded)) {
        return [
            'updatedAt' => gmdate('c'),
            'aliases' => [],
        ];
    }

    if (!is_array($decoded['aliases'] ?? null)) {
        $decoded['aliases'] = [];
    }

    $decoded['aliases'] = purge_expired_aliases($decoded['aliases']);
    return $decoded;
}

function save_alias_store(array $store): bool
{
    $dataDir = __DIR__ . DIRECTORY_SEPARATOR . 'data';
    if (!is_dir($dataDir) && !mkdir($dataDir, 0777, true) && !is_dir($dataDir)) {
        return false;
    }

    $store['updatedAt'] = gmdate('c');
    $json = json_encode($store, JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    if ($json === false) {
        return false;
    }

    return file_put_contents(aliases_path(), $json, LOCK_EX) !== false;
}

if (($_SERVER['HTTP_X_HFS_KEY'] ?? '') !== HFS_API_KEY) {
    http_response_code(403);
    echo json_encode(['ok' => false, 'error' => 'forbidden'], JSON_UNESCAPED_UNICODE);
    exit;
}

if (($_SERVER['REQUEST_METHOD'] ?? 'GET') !== 'POST') {
    http_response_code(405);
    echo json_encode(['ok' => false, 'error' => 'method_not_allowed'], JSON_UNESCAPED_UNICODE);
    exit;
}

$payload = json_decode(file_get_contents('php://input') ?: '', true);
if (!is_array($payload)) {
    http_response_code(400);
    echo json_encode(['ok' => false, 'error' => 'invalid_json'], JSON_UNESCAPED_UNICODE);
    exit;
}

$entries = $payload['entries'] ?? null;
if (!is_array($entries)) {
    http_response_code(400);
    echo json_encode(['ok' => false, 'error' => 'entries_required'], JSON_UNESCAPED_UNICODE);
    exit;
}

$instanceId = trim((string)($payload['instanceId'] ?? ''));
$replaceOwnEntries = (bool)($payload['replaceOwnEntries'] ?? false);

$normalizedEntries = [];
foreach ($entries as $entry) {
    if (!is_array($entry)) {
        continue;
    }

    $alias = slugify_alias((string)($entry['alias'] ?? ''));
    $targetBase = rtrim(trim((string)($entry['targetBase'] ?? '')), '/');
    $shareRoute = trim((string)($entry['shareRoute'] ?? ''));
    $name = trim((string)($entry['name'] ?? ''));
    $relayMode = trim((string)($entry['relayMode'] ?? ''));
    if ($relayMode !== 'android') {
        $relayMode = '';
    }

    if ($alias === '' || ($targetBase === '' && $relayMode === '')) {
        continue;
    }

    $normalizedEntries[$alias] = [
        'alias' => $alias,
        'targetBase' => $targetBase,
        'shareRoute' => $shareRoute,
        'name' => $name,
        'relayMode' => $relayMode,
        'instanceId' => $instanceId,
        'updatedAt' => gmdate('c'),
    ];
}

$store = load_alias_store();
$aliases = $store['aliases'];

if ($instanceId !== '' && $replaceOwnEntries) {
    foreach ($aliases as $key => $existing) {
        if (($existing['instanceId'] ?? '') === $instanceId) {
            unset($aliases[$key]);
        }
    }
}

foreach ($normalizedEntries as $alias => $entry) {
    if (isset($aliases[$alias])) {
        $owner = (string)($aliases[$alias]['instanceId'] ?? '');
        $relayMode = trim((string)($aliases[$alias]['relayMode'] ?? ''));
        $ownerAlive = $relayMode === 'android' ? instance_is_alive($owner) : true;
        if ($owner !== '' && $instanceId !== '' && $owner !== $instanceId && $ownerAlive) {
            http_response_code(409);
            echo json_encode([
                'ok' => false,
                'error' => 'alias_conflict',
                'alias' => $alias,
            ], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
            exit;
        }
    }

    $aliases[$alias] = $entry;
}

$store['aliases'] = $aliases;
if (!save_alias_store($store)) {
    http_response_code(500);
    echo json_encode(['ok' => false, 'error' => 'write_failed'], JSON_UNESCAPED_UNICODE);
    exit;
}

echo json_encode([
    'ok' => true,
    'count' => count($aliases),
    'aliases' => array_keys($aliases),
    'instanceId' => $instanceId,
], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
