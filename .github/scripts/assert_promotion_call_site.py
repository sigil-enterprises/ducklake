#!/usr/bin/env python3
"""ASSERT that the promotion gate is CALLED, in the publish job, before the upload.

The gate script's own controls prove every check FIRES. They say nothing about
whether anything RUNS it before the irreversible step, and that is the whole
difference between blocking a bad promotion and announcing one after the fact.

An earlier cut of this check grepped two `- name:` lines and compared line
numbers. Four realistic neuterings passed it at rc 0 - `continue-on-error: true`
on the step, the step's `run:` body replaced with `true`, `if: false`, and the
`- name:` line commented out - and it was not bounded by job, so a step in any
earlier job satisfied the ordering. A control that a one-line edit disarms is
not a control. This one parses the workflow and asserts on the STEP OBJECT:

  (i)   the `publish` job contains a step whose `run` invokes the gate script;
  (ii)  its index is lower than that of the step running `gh release upload`;
  (iii) both are in that SAME job;
  (iv)  the gate step carries no `if:` and no `continue-on-error:` - either one
        makes it advisory, and an advisory gate upstream of an irreversible
        step is the same thing as no gate.

A REFUSAL is exit non-zero WITH an `::error::` annotation. Exit non-zero
without one is a crash and says nothing about the workflow.
"""
import re
import sys

GATE_RE = re.compile(r"refuse_unproven_promotion\.sh")
UPLOAD_RE = re.compile(r"gh\s+release\s+upload")
JOB = "publish"


def annotate(msg):
    print(f"::error::{msg}")


def check(path):
    try:
        import yaml
    except ImportError:
        annotate(
            "PyYAML is not importable, so this workflow cannot be parsed and "
            "whether the promotion gate is wired in is UNKNOWN. Refusing: a "
            "checker that cannot read its subject must not report it correct."
        )
        return 1
    try:
        with open(path) as fh:
            wf = yaml.safe_load(fh)
    except FileNotFoundError:
        annotate(
            f"cannot read {path}, so whether the promotion gate is wired in at "
            "all is UNKNOWN. Refusing rather than reporting an unread file as "
            "correctly wired."
        )
        return 1
    except yaml.YAMLError as exc:
        annotate(
            f"{path} does not parse as YAML ({exc.__class__.__name__}), so no "
            "statement about its steps is possible. Refusing."
        )
        return 1

    jobs = (wf or {}).get("jobs")
    if not isinstance(jobs, dict):
        annotate(f"{path} declares no jobs. Refusing rather than passing a workflow with nothing in it.")
        return 1
    job = jobs.get(JOB)
    if not isinstance(job, dict):
        annotate(
            f"{path} has no `{JOB}` job, so this check has nothing to assert "
            "against. Refusing: the job was renamed and this assertion has "
            "silently stopped testing anything."
        )
        return 1
    steps = job.get("steps")
    if not isinstance(steps, list) or not steps:
        annotate(f"the `{JOB}` job in {path} declares no steps. Refusing.")
        return 1

    gate_i = upload_i = None
    for i, st in enumerate(steps):
        run = st.get("run") if isinstance(st, dict) else None
        if not isinstance(run, str):
            continue
        if gate_i is None and GATE_RE.search(run):
            gate_i = i
        if upload_i is None and UPLOAD_RE.search(run):
            upload_i = i

    if upload_i is None:
        annotate(
            f"the `{JOB}` job in {path} runs no `gh release upload`, so there "
            "is no irreversible step to order the gate against. Refusing: the "
            "upload moved and this assertion has silently stopped testing "
            "anything."
        )
        return 1
    if gate_i is None:
        annotate(
            f"REFUSING: no step in the `{JOB}` job of {path} runs "
            "refuse_unproven_promotion.sh. A gate nothing calls is a file, not "
            "a control, and a promotion gate that runs only after the release "
            "is published announces a bad promotion rather than preventing the "
            "asset that makes it usable."
        )
        return 1
    if gate_i > upload_i:
        annotate(
            f"REFUSING: the promotion gate is step {gate_i} and the upload is "
            f"step {upload_i}, so the gate runs AFTER it. An upload is "
            "irreversible and a gate downstream of an irreversible step is not "
            "a gate: the asset is already on the release when the job goes "
            "red, and a red job does not take it off."
        )
        return 1

    gate = steps[gate_i]
    for key, why in (
        ("if", "a conditional step is skipped rather than run, and a skipped gate exits ZERO"),
        ("continue-on-error", "an errored step that does not fail the job lets the very next step upload the asset"),
    ):
        if key in gate:
            annotate(
                f"REFUSING: the promotion gate step carries `{key}: "
                f"{gate[key]!r}`, which makes it advisory - {why}. An advisory "
                "gate upstream of an irreversible step is the same thing as no "
                "gate."
            )
            return 1

    run = gate["run"]
    if not re.search(r"(^|\n)\s*bash\s+[^\n]*refuse_unproven_promotion\.sh", run):
        annotate(
            "REFUSING: the gate step mentions refuse_unproven_promotion.sh but "
            "does not invoke it with bash. A step that names the script in a "
            "comment while running something else is the exact neutering this "
            "check exists to catch."
        )
        return 1

    print(f"{path}: `{JOB}` runs the promotion gate at step {gate_i}, uploads at step {upload_i} - gate is upstream, unconditional, and fails the job.")
    return 0


# ------------------------------------------------------------------ controls --
#
# This assertion is absence-shaped - no missing call, no bad ordering - so it
# would read as a pass if it were broken. Each fixture below is the real
# workflow with exactly one realistic neutering applied to the PARSED structure
# (not to the text, so a fixture cannot fail to apply and quietly report green),
# and each must be REFUSED.
def selftest(real):
    import copy
    import os
    import tempfile

    import yaml

    with open(real) as fh:
        base = yaml.safe_load(fh)

    fails = 0
    tmp = tempfile.mkdtemp()

    def gate_index(doc, job=JOB):
        for i, st in enumerate(doc["jobs"][job]["steps"]):
            r = st.get("run")
            if isinstance(r, str) and GATE_RE.search(r):
                return i
        return None

    def case(want, label, reason, mutate=None, path=None):
        nonlocal fails
        if path is None:
            doc = copy.deepcopy(base)
            mutate(doc)
            path = os.path.join(tmp, label.replace(" ", "_")[:40] + ".yml")
            with open(path, "w") as fh:
                yaml.safe_dump(doc, fh, sort_keys=False)
        import io
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with redirect_stdout(buf):
            st = check(path)
        out = buf.getvalue()
        if st != want:
            print(f"FAIL  {label}: expected exit {want}, got {st}")
            print("\n".join("      | " + l for l in out.splitlines()))
            fails += 1
            return
        if want and "::error::" not in out:
            print(f"FAIL  {label}: exit {st} with no ::error:: - a crash, not a refusal")
            fails += 1
            return
        if want and reason not in out:
            print(f"FAIL  {label}: refused, but not for the condition under test ({reason})")
            print("\n".join("      | " + l for l in out.splitlines()))
            fails += 1
            return
        print(f"PASS  {label} (exit {st})")

    # Green must be reachable against the REAL file, or every red below is
    # unattributable.
    case(0, "the real workflow calls the gate before the upload", "", path=real)

    def drop(doc):
        i = gate_index(doc)
        del doc["jobs"][JOB]["steps"][i]
    case(1, "the gate step is deleted", "no step in the `publish` job", drop)

    def neuter(doc):
        i = gate_index(doc)
        doc["jobs"][JOB]["steps"][i]["run"] = "true  # refuse_unproven_promotion.sh\n"
    case(1, "the gate step's run body no longer runs the script", "does not invoke it with bash", neuter)

    def coe(doc):
        doc["jobs"][JOB]["steps"][gate_index(doc)]["continue-on-error"] = True
    case(1, "continue-on-error: true on the gate step", "continue-on-error", coe)

    def iff(doc):
        doc["jobs"][JOB]["steps"][gate_index(doc)]["if"] = False
    case(1, "if: false on the gate step", "`if:", iff)

    def moved(doc):
        i = gate_index(doc)
        step = doc["jobs"][JOB]["steps"].pop(i)
        doc["jobs"][JOB]["steps"].append(step)
    case(1, "the gate step moved after the upload", "runs AFTER it", moved)

    def other_job(doc):
        i = gate_index(doc)
        step = doc["jobs"][JOB]["steps"].pop(i)
        other = next(j for j in doc["jobs"] if j != JOB)
        doc["jobs"][other]["steps"].append(step)
    case(1, "the gate step moved to a DIFFERENT job", "no step in the `publish` job", other_job)

    def no_upload(doc):
        steps = doc["jobs"][JOB]["steps"]
        for i, st in enumerate(steps):
            r = st.get("run")
            if isinstance(r, str) and UPLOAD_RE.search(r):
                del steps[i]
                return
    case(1, "the upload step is gone", "runs no `gh release upload`", no_upload)

    def no_job(doc):
        del doc["jobs"][JOB]
    case(1, "the publish job is renamed away", "has no `publish` job", no_job)

    bad = os.path.join(tmp, "bad.yml")
    with open(bad, "w") as fh:
        fh.write("jobs:\n  publish:\n   steps:\n  - : : :\n")
    case(1, "the workflow does not parse", "does not parse as YAML", path=bad)
    case(1, "the workflow cannot be read", "cannot read", path=os.path.join(tmp, "nope.yml"))

    print()
    if fails:
        print(f"::error::promotion call-site self-test: {fails} case(s) failed")
        return 1
    print("promotion call-site self-test: all cases passed")
    return 0


if __name__ == "__main__":
    args = sys.argv[1:]
    target = ".github/workflows/Release.yml"
    if args and args[0] == "--selftest":
        sys.exit(selftest(args[1] if len(args) > 1 else target))
    sys.exit(check(args[0] if args else target))
