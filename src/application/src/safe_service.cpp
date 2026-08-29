// SafeService: create/unlock/lock, смена пароля, authorize.
// мьютексы: lifecycle - create/unlock/lock по очереди (argon2 по 256 МБ параллельно
// гонять незачем), state - короткий доступ к current_/epoch_, password - смена пароля.
// authorize берет только state
