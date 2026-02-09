# Security Notes

## API Key Management

**NEVER commit API keys to git!**

### Secure Storage:

1. **For local development**: Store keys in `.dev.vars` (gitignored)
2. **For production**: Use `wrangler secret put` to store in Cloudflare
3. **For testing**: Use environment variables or `.env` file (gitignored)

### What NOT to do:

❌ Hardcode API keys in source code
❌ Commit `.env` or `.dev.vars` files
❌ Share API keys in chat/email

### If a Key is Exposed:

1. **Immediately regenerate** the key at the provider's dashboard
2. **Remove from git history** (if needed, use `git filter-repo` or BFG Repo-Cleaner)
3. **Update** all services using the old key
4. **Monitor** API usage for suspicious activity

## Previous Security Incident

**2026-02-09**: WeatherAPI.com key was accidentally committed in `test.js` and detected by GitGuardian. Key was regenerated and code fixed to use environment variables.
