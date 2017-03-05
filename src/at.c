#include <ctype.h>
#include <string.h>

#include "at.h"
#include "shared/log.h"

int at_parse(char *str, struct at_command *cmd)
{
	memset(cmd->command, 0, sizeof(cmd->command));
	memset(cmd->value, 0, sizeof(cmd->value));

	/* Skip initial whitespace */
	const char *s = str;
	while (isspace(*s))
		s++;

	/* Remove trailing whitespace */
	char *end = str + strlen(str) - 1;
	while(end >= s && isspace(*end))
		*end-- = '\0';

	/* starts with AT? */
	if (strncasecmp(s, "AT", 2))
		return -1;

	/* Can we find equals sign? */
	char *equal = strstr(s, "=");
	if (equal != NULL)
	{
		/* Set (ATxxx=value or test (ATxxx=?) */
		strncpy(cmd->command, s + 2, equal - s - 2);

		if (equal[1] == '?')
		{
			cmd->type = AT_CMD_TYPE_TEST;
		}
		else
		{
			cmd->type = AT_CMD_TYPE_SET;
			strncpy(cmd->value, equal + 1, AT_MAX_VALUE_SIZE - 1);
		}
	}
	else
	{
		/* Get (ATxxx?) */
		cmd->type = AT_CMD_TYPE_GET;
		char *question = strstr(s, "?");
		if (question != NULL )
			strncpy(cmd->command, s + 2, question - s - 2);
		else
			return -1;
	}
	debug("Got %s\ntype = %d\ncommand = %s\nvalue = %s", str, cmd->type, cmd->command, cmd->value);
	return 0;
}
