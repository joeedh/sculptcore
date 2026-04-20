#!/usr/bin/env node
import fs from "fs";
import Path from "path";
import child_process from "child_process";

const cmd = process.argv[2] ?? "--help";
const EMSDK_VERSION = fs.readFileSync("./emsdkVersion.txt", "utf-8").trim();

function run(cmd) {
  return child_process.execSync(cmd, { shell: true, stdio: "inherit" });
}

const CMAKE_ARGS = `-DBUILD_WASM=ON -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`;
switch (cmd) {
  case "--help":
    console.log("Usage: node make.mjs <build|clean|configure> [emsdk|native]");
    break;
  case "configure":
    if (!fs.existsSync("build")) {
      fs.mkdirSync("build");
    }
    if (process.argv[3] === "native") {
      if (!fs.existsSync("build/native")) {
        fs.mkdirSync("build/native");
      }
      run("node configureEnv.mjs --output-env-bat");
      run("node configureEnv.mjs --output-env-bash");
      run(`cd build/native && node ../../configureEnv.mjs cmake ../.. -G Ninja `);
    } else {
      run(`cd build && emcmake cmake .. ${CMAKE_ARGS}`);
    }
    break;
  case "build":
    console.log("Building...");
    if (process.argv[3] === 'native') {
      run(`cd build/native && node ../../configureEnv.mjs cmake --build .`);
    } else {
    // ensure final linked files are destroyed
    // since emscripten is not that great at making
    // errors during complication actually be obvious
    if (fs.existsSync("build/sculptcore.js")) {
      fs.rmSync("build/sculptcore.js", { force: true });
    }
    if (fs.existsSync("build/sculptcore.wasm")) {
      fs.rmSync("build/sculptcore.wasm", { force: true });
    }
    run(`cd build && cmake --build . `);
    }
    break;
  case "clean":
    console.log("Cleaning...");
    if (process.argv[3] === 'native') {
      run(`cd build/native && ninja clean`);
    } else {
      run(`cd build && ninja clean`);
    }
    break;
  case "install-emsdk":
    console.log("Installing emsdk...");
    run("git submodule init");
    run("git submodule update");
    run(
      `cd emsdk && bash emsdk install ${EMSDK_VERSION} && bash emsdk install cmake-4.2.0-rc3-64bit ninja-git-release-64bit `,
    );
    break;
  default:
    console.log(`Unknown command: ${cmd}`);
    break;
}
