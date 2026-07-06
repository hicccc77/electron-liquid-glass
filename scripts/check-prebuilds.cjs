// Pre-publish guard: verify the win32-x64 prebuilt binary exists and is
// resolvable by node-gyp-build, so we never publish a package without a
// binary (consumers would get isSupported() === false forever).
const { existsSync, readdirSync } = require('fs')
const { join } = require('path')

const dir = join(__dirname, '..', 'prebuilds', 'win32-x64')
if (!existsSync(dir) || !readdirSync(dir).some((f) => f.endsWith('.node'))) {
    console.error('[prepublish] missing prebuilds/win32-x64/*.node, run: npm run prebuilds')
    process.exit(1)
}
console.log('[prepublish] prebuilds OK:', readdirSync(dir).join(', '))
