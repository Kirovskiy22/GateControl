from services.snmp_worker import SNMPWorker


def test_open_close_cycle() -> None:
    worker = SNMPWorker()

    try:
        initial = worker.get_state()
        worker.open_gate()
        after_open = worker.get_state()
        worker.close_gate()
        after_close = worker.get_state()

        print(f"initial={initial}, after_open={after_open}, after_close={after_close}")
    finally:
        worker.shutdown()


if __name__ == "__main__":
    test_open_close_cycle()
