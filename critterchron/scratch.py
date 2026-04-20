from compiler import CritCompiler
c = CritCompiler()
c.compile("agents/ants.crit")

def validate_loops(ir):
    for agent, instructions in ir["behaviors"].items():
        def check_path(pc, visited):
            if pc in visited:
                # infinite loop without done!
                # Wait, if we loop, did we hit a done? done resets pc to 0 and returns!
                # If we loop without hitting done, that's an error.
                return False
            
            if pc >= len(instructions):
                # Fell off the bottom without hitting done!
                return False
            
            indent, inst = instructions[pc]
            if inst == "done":
                return True
                
            if inst.startswith("if "):
                # True branch falls through to pc + 1
                true_safe = check_path(pc + 1, visited | {pc})
                if not true_safe: return False
                
                # False branch jumps to next instruction with indent <= current_indent
                false_pc = pc + 1
                while false_pc < len(instructions) and instructions[false_pc][0] > indent:
                    false_pc += 1
                false_safe = check_path(false_pc, visited | {pc})
                if not false_safe: return False
                
                return True
                
            elif inst == "else:":
                # If we naturally reach an else, we should jump past it!
                # (since we just finished the True branch of an if)
                # Wait! A natural reach of else means we finished the True branch.
                # The jump target is the next instruction with indent <= else_indent
                skip_pc = pc + 1
                while skip_pc < len(instructions) and instructions[skip_pc][0] > indent:
                    skip_pc += 1
                return check_path(skip_pc, visited | {pc})
                
            else:
                return check_path(pc + 1, visited | {pc})

        if not check_path(0, set()):
            print(f"Agent {agent} has a path that falls off the bottom!")

validate_loops(c.ir)
