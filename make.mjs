#!/usr/bin/env node
import fs from "fs";
import child_process from "child_process";
import yargs from "yargs";
import { hideBin } from "yargs/helpers";

const EMSDK_VERSION = fs.readFileSync("./emsdkVersion.txt", "utf-8").trim();
const CMAKE_ARGS = `-DBUILD_WASM=ON -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`;

function run(cmd) {
  try {
    return child_process.execSync(cmd, { shell: true, stdio: "inherit" });
  } catch (error) {
    process.stderr.write(error.message + "\n");
    process.exit(1);
  }
}

function ensureDir(p) {
  if (!fs.existsSync(p)) fs.mkdirSync(p, { recursive: true });
}

function buildDir(target) {
  return target === "native" ? "build/native" : "build";
}

// Returns the `node ../configureEnv.mjs [--emsdk]` prefix used inside buildDir.
function envPrefix(target) {
  const rel = target === "native" ? "../.." : "..";
  const emsdk = target === "native" ? "" : "--emsdk ";
  return `node ${rel}/configureEnv.mjs ${emsdk}`.trimEnd();
}

const targetPositional = (y) =>
  y.positional("target", {
    choices: ["wasm", "native"],
    default: "wasm",
    describe: "Build target",
  });

yargs(hideBin(process.argv))
  .scriptName("make.mjs")
  .command(
    "configure [target]",
    "Configure the build",
    targetPositional,
    ({ target }) => {
      ensureDir("build");
      const dir = buildDir(target);
      ensureDir(dir);
      const env = envPrefix(target);
      if (target === "native") {
        run(`cd ${dir} && ${env} cmake ../.. -G Ninja `);
      } else {
        run(`cd ${dir} && ${env} emcmake cmake .. ${CMAKE_ARGS}`);
      }
    },
  )
  .command("build [target]", "Build", targetPositional, ({ target }) => {
    console.log("Building...");
    const dir = buildDir(target);
    const env = envPrefix(target);
    if (target === "wasm") {
      // ensure final linked files are destroyed
      // since emscripten is not that great at making
      // errors during complication actually be obvious
      if (fs.existsSync("build/sculptcore.js")) {
        fs.rmSync("build/sculptcore.js", { force: true });
      }
      if (fs.existsSync("build/sculptcore.wasm")) {
        fs.rmSync("build/sculptcore.wasm", { force: true });
      }
    }
    run(`cd ${dir} && ${env} cmake --build . `);
  })
  .command("clean [target]", "Clean build dir", targetPositional, ({ target }) => {
    console.log("Cleaning...");
    run(`cd ${buildDir(target)} && ${envPrefix(target)} ninja clean`);
  })
  .command("test [target]", "Run ctest", targetPositional, ({ target }) => {
    console.log("Testing...");
    run(`cd ${buildDir(target)} && ${envPrefix(target)} ctest .`);
  })
  .command("install-emsdk", "Install pinned emsdk", {}, () => {
    console.log("Installing emsdk...");
    run("git submodule init");
    run("git submodule update");
    run(
      `cd emsdk && bash emsdk install ${EMSDK_VERSION} && bash emsdk install cmake-4.2.0-rc3-64bit ninja-git-release-64bit `,
    );
    run(`cd emsdk && bash emsdk activate ${EMSDK_VERSION} cmake-4.2.0-rc3-64bit ninja-git-release-64bit --permanent `)
    // emsdk annoyingly is missing a .gitignore for their cmake binary folder
    fs.appendFileSync('emsdk/.gitignore', '\ncmake\n')
  })
  .demandCommand(1, "Specify a command (see --help)")
  .strict()
  .help()
  .parse();
