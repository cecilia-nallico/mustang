# AGENTS.md

Guidance for AI coding agents working in this repository.

## Agent behavior

Read and apply `docs/agents/behavior.md`

## Code style and tools

Read and apply `docs/agents/styleguide.md`



The repo has a **split build** (since 2026-10-26):

- **Bazel** (bzlmod, root `MODULE.bazel`) builds the **C++** and
  **Java** code ONLY: the C++ walleye service (`walleye/`), the C++
  maintenance CLI (`walleye/infra/cli/`) and the official-OTel-SDK
  Java reference probe (`java/probe/`, rules_jvm_external Maven deps).
- The **docs site** builds natively from `docs/`: `make -C docs html`
  (pip venv + Sphinx — `docs/Makefile`, locked wheels in
  `docs/requirements.lock.txt`).
- The **frontend** builds with **plain npm** from `frontend/`
  (`npm install / dev / typecheck / build` — see `frontend/README.md`).
- **No Dockerfiles, no Docker daemon**: every OCI image (walleye,
  docs, frontend) is built with **buildah** from the per-component
  `Containerfile` (`walleye/Containerfile`, `docs/Containerfile`,
  `frontend/Containerfile`) via `<component>/scripts/build-image.sh`
  / `export-image.sh`. The walleye image is Debian trixie with a
  **modern OpenSSL from apt** — there is no runtime libssl layer
  anymore.

```bash
# C++ (bazel)
bazel build //walleye:walleye
bazel build //walleye/infra/cli:walleye
bazel test  //walleye/infra/cli:walleye_test
bazel build //java/probe:walleye-probe-java   # docs/development/probe.md

# docs (native)
make -C docs html

# images (buildah)
walleye/scripts/build-image.sh    # bazel binary -> nallico/walleye:latest
docs/scripts/build-image.sh       # make html -> nallico/docs:latest
frontend/scripts/build-image.sh   # npm build -> walleye-frontend:local
```

## Documentation (`docs/`)

**Keep the docs up to date with every change.** A change that alters
behavior, infra, tooling or UX is not done until the corresponding docs
page reflects it — update it in the same commit.

The docs have two audiences, split deliberately: human sections
(`intro`, `runbooks`, `development/*`, `infra/*`) are concise and
curated; the **AI plans** section surfaces `agents/plans/*.md`
verbatim as verbose machine memory for coding agents. Conventions and
mechanics: `docs/AGENTS.md`.

## Frontend (`frontend/`)

The frontend has its own standard build (Vite + React Router v8, SSR,
TypeScript) — **plain npm, no Bazel**. From `frontend/`:

```bash
npm install        # install dependencies
npm run dev        # dev server with HMR (http://localhost:5173)
npm run build      # protobuf codegen + production build
npm run typecheck  # protobuf codegen + react-router typegen + tsc
npm run start      # serve the production build (http://localhost:3000)
npm run image      # buildah OCI image (scripts/build-image.sh)
npm run image:export  # docker-archive tarball (scripts/export-image.sh)
```

Docker image (buildah + `frontend/Containerfile`, distroless runtime;
no Docker daemon, no Bazel):

```bash
./frontend/scripts/build-image.sh [tag]    # default walleye-frontend:local
./frontend/scripts/export-image.sh [out.tar]
./frontend/scripts/run-image.sh            # buildah from/run, host net
```

Bazel notes: version pinned in `.bazelversion` (use bazelisk); commit
`MODULE.bazel.lock`. Bazel covers only the C++/Java targets above.

All frontend operations (install/dev/typecheck/proto/build/serve/image)
have wrapper scripts in `frontend/scripts/` — see `frontend/README.md`.

Protobuf schemas live in the repo-root `proto/` directory (shared between
frontend and backend) and are compiled to TypeScript into `frontend/app/gen/`
(gitignored) via `npm run buf:generate` in `frontend/` (Buf + protobuf-ES).
Needs Node >= 22.22.

**The frontend uses supabase.**

Frontend uses supbase base. It implements sign-up/sign-in logic.
For more complicated queries, it should call the walleye C++ server via gRPC.

## Agent Workflow

1. Read this file and any nearby `AGENTS.md` in subdirectories before editing.
2. Follow existing code in the repo as the tiebreaker for style questions.
3. Read and apply agents/CPP.md.
4. Never reformat or rewrite files unrelated to your task.
5. Keep commits/diffs minimal and focused; no drive-by refactors.
6. If a build/test command fails or is missing, stop and report rather than guessing.
7. Comment *why*, not *what*. Don't narrate the code line by line —
   the code already says what it does. A comment earns its place only
   when it records a non-obvious reason, constraint, tradeoff, or
   live-ops lesson that the code cannot express.