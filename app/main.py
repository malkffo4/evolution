#!/usr/bin/env python3

from core.manager import EvolutionManager

def main():
    manager = EvolutionManager()

    try:
        manager.initialize()
        manager.run()
    except KeyboardInterrupt:
        print("\n[SYSTEM] Shutdown requested.")
    finally:
        manager.shutdown()

if __name__ == "__main__":
    main()
