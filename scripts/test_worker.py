from services.snmp_worker import SNMPWorker


def main() -> None:
    worker = SNMPWorker()

    try:
        print("\n=== ШАГ 1: GET ===")
        state = worker.get_state()
        print(f"GET OK, состояние = {state}")

        print("\n=== ШАГ 2: SET 0 ===")
        worker.open_gate()
        print("SET 0 OK")

        print("\n=== ШАГ 3: GET ===")
        state = worker.get_state()
        print(f"GET OK, состояние = {state}")

        print("\n=== ШАГ 4: SET 1 ===")
        worker.close_gate()
        print("SET 1 OK")

        print("\n=== ШАГ 5: GET ===")
        state = worker.get_state()
        print(f"GET OK, состояние = {state}")
    finally:
        print("\n=== SHUTDOWN ===")
        worker.shutdown()
        print("Worker остановлен")


if __name__ == "__main__":
    main()
