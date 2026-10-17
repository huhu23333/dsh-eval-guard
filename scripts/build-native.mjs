#!/usr/bin/env node
/** Compile the seccomp supervisor for this host. */
import { execFileSync } from 'node:child_process'
import { existsSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = dirname(dirname(fileURLToPath(import.meta.url)))
const source = join(root, 'native', 'supervisor.c')
const output = join(root, 'native', 'dsh-eval-supervisor')
if (!existsSync(source)) throw new Error('missing ' + source)
execFileSync('gcc', ['-O2', '-Wall', '-o', output, source], { stdio: 'inherit' })
console.log('built ' + output)
