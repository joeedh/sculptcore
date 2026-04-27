if (process.platform !== "win32") {
  console.log("No action needed");
  process.exit(0);
}

import fs from "fs";
import Path from "path";
import child_process from "child_process";
import { fileURLToPath } from "url";

function getVSEnv() {
  const PROGFILES = process.env.ProgramFiles;
  const APPDATA = process.env.APPDATA;
  const USERPROFILE = process.env.USERPROFILE;

  const { join } = Path;

  const VSPath = join(PROGFILES, "Microsoft Visual Studio");

  if (!fs.existsSync(VSPath)) {
    process.stderr.write("Visual Studio is not installed\n");
    process.exit(-1);
  }

  const CWD = process.cwd();

  const dir = fs
    .readdirSync(VSPath)
    .filter((d) => fs.statSync(join(VSPath, d)).isDirectory());

  const versionMap = new Map([
    [2022, 17],
    [2026, 18],
    [2020, 16],
  ]);

  const getVersion = (d) => versionMap.get(d) ?? d;

  const versions = dir
    .map((d) => parseInt(d))
    .filter((d) => !isNaN(d))
    .sort((a, b) => getVersion(b) - getVersion(a));

  const version = versions[0];
  if (version === undefined) {
    process.stderr.write("No Visual Studio version found\n");
    process.exit(-1);
  }

  //C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build

  const path = join(
    VSPath,
    "" + version,
    "Community",
    "VC",
    "Auxiliary",
    "Build",
    "vcvars64.bat",
  );
  process.chdir(join(VSPath, "" + version, "Community"));

  if (!fs.existsSync(path)) {
    process.stderr.write("Could not find vcvars64.bat\n");
    process.stderr.write(`  tried "${path}"\n`);
    process.exit(-1);
  }

  // vcvars64.bat chokes with "\Windows was unexpected at this time." when
  // the inherited PATH is in Git-Bash form (":"-separated, "/c/..." paths).
  // Give it a clean, minimal Windows PATH — vcvars builds its own PATH from
  // scratch anyway, so this is sufficient. Also drop env var names cmd
  // can't parse.
  const systemRoot = process.env.SystemRoot || "C:\\Windows";
  const childEnv = {};
  for (const [k, v] of Object.entries(process.env)) {
    if (!k || !/^[A-Za-z_][A-Za-z0-9_()]*$/.test(k)) continue;
    childEnv[k] = v;
  }
  childEnv.PATH = [
    `${systemRoot}\\System32`,
    systemRoot,
    `${systemRoot}\\System32\\Wbem`,
  ].join(";");
  const result = child_process.execSync(
    `cmd /s /c \"call \"${path}\" && set\"`,
    { env: childEnv },
  );
  const env = result
    .toString("latin1")
    .split("\n")
    .filter(
      (l) =>
        !l.startsWith("**") &&
        !l.trim().toLowerCase().startsWith("[vcvarsall.bat]"),
    )
    .join("\n");

  process.chdir(CWD);
  return env;
}

function getEmsdkEnv() {
  const CWD = process.cwd();
  const scriptPath = fileURLToPath(import.meta.url);

  process.chdir(Path.dirname(scriptPath));
  process.chdir("emsdk");

  delete process.env.EMSDK_QUIET;
  const childEnv = { ...process.env };

  if (process.platform === "win32") {
    // we do not want cygpaths
    delete childEnv.MSYSTEM;
  }

  const result = child_process.execSync("python emsdk.py construct_env", {
    stdio: "pipe",
    shell: false,
    detached: false,
    env: childEnv,
  });

  let env;
  if (process.platform === "win32") {
    env = fs
      .readFileSync("emsdk_set_env.bat", "utf8")
      .replace(/\r/g, "")
      .replace(/\n\n+/g, "\n")
      .split("\n")
      .map((l) => {
        if (l.toLowerCase().startsWith("set ")) {
          l = l.slice(4);
        }
        return l;
      })
      .join("\n");
  } else {
    if (!result) {
      process.stderr.write("Failed to get emsdk environment\n");
      process.exit(-1);
    }
    env = result
      .toString("utf8")
      .replace(/\r/g, "")
      .split("\n")
      .map((l) => l.trim())
      .filter((l) => l.length > 0)
      .map((l) => {
        if (l.toLowerCase().startsWith("export ")) {
          l = l.slice(7);
        } else if (l.toLowerCase().startsWith("set ")) {
          l = l.slice(4);
        }
        return l.trim();
      })
      .join("\n");
  }

  process.chdir(CWD);
  return env;
}

let args = process.argv.slice(2);
let target = "native";

if (args[0] === "--emsdk") {
  target = "emsdk";
  args = args.slice(1);
}

let env;
if (target === "native") {
  env = process.platform === "win32" ? getVSEnv() : process.env;
} else {
  env = getEmsdkEnv();
}

if (process.argv.includes("--output-env")) {
  process.stdout.write(env);
  process.exit(0);
}

for (const line of env.replace(/\r/g, "").split("\n")) {
  if (!line) continue;
  const eq = line.indexOf("=");
  if (eq <= 0) continue;
  process.env[line.slice(0, eq)] = line.slice(eq + 1);
}

const proc = child_process.spawn(args[0] ?? "cmd", [...args.slice(1)], {
  stdio: "inherit",
  shell: false,
});
proc.on("close", (code, signal) => {
  process.exit(code);
});
proc.on("exit", (code, signal) => {
  process.exit(code);
});
