from pathlib import Path
from typing import Optional
import subprocess

def run_command(command: str, cwd: Optional[str | Path] = None) -> str:
    result = subprocess.run(
        command,
        shell=True,
        cwd=str(cwd) if cwd is not None else None,
        capture_output=True,
        tst=True
    )
    
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip())
    
    return result.stdout.strip()

def main():
    side = ['server-side','client-side'][int(input("Which side [server-side/client-side] (0/1) : "))]
    file = input("Enter file name (without extension): ")
    
    run_command(
        command=f"g++ -std=c++17 packages/{side}/{file} {side}/{file}.cpp -pthread"
    )