from pysnmp.hlapi.v3arch.asyncio import (
    CommunityData,
    ContextData,
    Integer,
    ObjectIdentity,
    ObjectType,
    SnmpEngine,
    UdpTransportTarget,
    get_cmd,
    set_cmd,
)

from config import Config

cfg = Config()


class SNMPGate:
    """SNMP-клиент для управления DO1 на SNR-ERD-2.3."""

    def __init__(self):
        self.engine = SnmpEngine()
        self.community = CommunityData(cfg.community, mpModel=0)
        self.context = ContextData()

    async def _get_target(self):
        return await UdpTransportTarget.create((cfg.ip, cfg.port))

    async def get_state(self) -> int:
        target = await self._get_target()

        error_indication, error_status, error_index, var_binds = await get_cmd(
            self.engine,
            self.community,
            target,
            self.context,
            ObjectType(ObjectIdentity(cfg.oid)),
        )

        if error_indication:
            raise RuntimeError(str(error_indication))

        if error_status:
            raise RuntimeError(error_status.prettyPrint())

        for var in var_binds:
            return int(var[1])

        raise RuntimeError("SNMP не вернул значение")

    async def set_state(self, value: int) -> None:
        if value not in (0, 1, 2):
            raise ValueError("Допустимые значения DO1: 0, 1 или 2")

        target = await self._get_target()

        error_indication, error_status, error_index, var_binds = await set_cmd(
            self.engine,
            self.community,
            target,
            self.context,
            ObjectType(ObjectIdentity(cfg.oid), Integer(value)),
        )

        if error_indication:
            raise RuntimeError(str(error_indication))

        if error_status:
            raise RuntimeError(error_status.prettyPrint())

    async def open_gate(self) -> None:
        await self.set_state(0)

    async def close_gate(self) -> None:
        await self.set_state(1)

    async def pulse_gate(self) -> None:
        """Импульс 3 секунды (режим Reset на ERD)."""
        await self.set_state(2)

    def close(self) -> None:
        self.engine.close_dispatcher()
