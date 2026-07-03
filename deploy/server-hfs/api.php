<?php
declare(strict_types=1);

header('Content-Type: application/json; charset=utf-8');

const HFS_API_KEY = 'vnc-hfs-20260629';
const HFS_ALIAS_TTL_SECONDS = 300;

function slugify_alias(string $value): string
{
    $value = trim(mb_strtolower($value, 'UTF-8'));
    $value = preg_replace('/[^a-z0-9]+/u', '-', $value) ?? '';
    $value = trim($value, '-');
    return $value;
}

function aliases_path(): string
{
    $baseDir = defined('HFS_STORE_DIR') && is_string(HFS_STORE_DIR) && HFS_STORE_DIR !== ''
        ? HFS_STORE_DIR
        : __DIR__ . DIRECTORY_SEPARATOR . 'data';
    return $baseDir . DIRECTORY_SEPARATOR . 'aliases.json';
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
        if ($updatedAt === false || ($now - $updatedAt) > HFS_ALIAS_TTL_SECONDS) {
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
    $dataDir = defined('HFS_STORE_DIR') && is_string(HFS_STORE_DIR) && HFS_STORE_DIR !== ''
        ? HFS_STORE_DIR
        : __DIR__ . DIRECTORY_SEPARATOR . 'data';
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

$submittedAliases = [];
$normalizedEntries = [];
foreach ($entries as $entry) {
    if (!is_array($entry)) {
        continue;
    }

    $alias = slugify_alias((string)($entry['alias'] ?? ''));
    $targetBase = trim((string)($entry['targetBase'] ?? ''));
    $shareRoute = trim((string)($entry['shareRoute'] ?? ''));
    $name = trim((string)($entry['name'] ?? ''));

    if ($alias === '' || $targetBase === '') {
        continue;
    }

    $submittedAliases[] = $alias;
    $normalizedEntries[$alias] = [
        'alias' => $alias,
        'targetBase' => rtrim($targetBase, '/'),
        'shareRoute' => $shareRoute,
        'name' => $name,
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
        if ($owner !== '' && $instanceId !== '' && $owner !== $instanceId) {
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
