#!/usr/bin/env php
<?php

/*
 *  sync.php - SFS Asynchronous filesystem replication
 *
 *  Copyright © 2014-2015  Immobiliare.it S.p.A.
 *
 *  This file is part of SFS.
 *
 *  SFS is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  SFS is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with SFS.  If not, see <http://www.gnu.org/licenses/>.
 */

declare(ticks = 1);

error_reporting (E_ALL & ~E_STRICT);
ini_set ("display_errors", 1);
ini_set ("error_log", "syslog");

define('ESTIMATED_BATCH_NAME_LENGTH', 150);

function sync_shutdown () {
	global $sync;
	$sync->shutdown ();
	exit(0);
}

class Sync {
	public $MSG_PUSH = 1;
	public $MSG_PULL = 2;
	public $MSG_RESULT = 3;

	public function __construct ($configPath, $pidPath) {
		$this->configPath = $configPath;
		$this->pidPath = $pidPath;
		$this->nodeTimeout = array ();

		global $CONFIG;
		require $this->configPath;
		if (!$this->setConfig ($CONFIG)) {
			echo "Could not load config, see syslog\n";
			return;
		}
		$this->reopenLog ("main");
	}

	public function shutdown () {
		if (!empty ($this->didShutdown)) {
			return;
		}
		$this->didShutdown = TRUE;
		if ($this->pidPath) {
			unlink($this->pidPath);
		}
		$this->removeShm ();
		syslog(LOG_NOTICE, "Shutdown successful");
	}

	/* all processes */
	public function setConfig ($config, $subident=null) {
		// display messages only in sched process
		if (empty($config["SYNC_DATA_REC"]) || empty($config["SYNC_DATA_NOREC"])) {
			if ($subident == "sched") {
				// display the message only once in the syslog in the parent process
				syslog(LOG_CRIT, "Sync data command not configured");
			}
			return FALSE;
		}

		if (!empty ($this->config) && array_keys($this->config["NODES"]) != array_keys($config["NODES"])) {
			if ($subident == "sched") {
				// display
				syslog(LOG_CRIT, "Nodes cannot change at runtime");
			}
			return FALSE;
		}

		if (empty ($this->config["PULL_BATCHES"])) {
			if ($subident == "sched") {
				// display the message only once in the syslog in the parent process
				syslog(LOG_WARNING, "Pull batches command not configured");
			}
		}

		$this->config = $config;
		return TRUE;
	}

	/* all processes */
	public function reopenLog ($subident="") {
		closelog ();
		$ident = str_replace ("%n", $subident, $this->config["LOG_IDENT"]);
		if (version_compare(phpversion(), '5.5.0', '>=')) {
			cli_set_process_title ($ident);
		}
		openlog($ident, $this->config["LOG_OPTIONS"], $this->config["LOG_FACILITY"]);
	}

	/* all processes */
	public function reloadConfig ($subident) {
		global $CONFIG;
		$curConfig = $CONFIG;
		try {
			require $this->configPath;
			if ($curConfig == $CONFIG) {
				return;
			}

			if (!$this->setConfig ($CONFIG, $subident)) {
				return;
			}

			$this->reopenLog ($subident);
			syslog(LOG_NOTICE, "Configuration reloaded successfully");
		} catch (Exception $e) {
			syslog(LOG_WARNING, "Error reloading config: ".$e->getMessage ());
		}
	}

	/* main process */
	public function setupShmKeys () {
		$tempnam = tempnam("/tmp", "php-sync");
		$num = 1;
		$this->queueKey = ftok($tempnam, $num++);
		$this->semKeys = array();
		foreach (array_keys ($this->config["NODES"]) as $node) {
			$this->semKeys[$node] = ftok($tempnam, $num++);
		}
		unlink($tempnam);
	}

	/* main process */
	public function setupShm () {
		$this->queue = msg_get_queue ($this->queueKey);
		if (msg_set_queue($this->queue, array('msg_qbytes' => ESTIMATED_BATCH_NAME_LENGTH*$this->config["BULK_MAX_BATCHES"])) === FALSE) {
			syslog(LOG_CRIT, "Error adjusting queue message size");
		}

		$this->sems = array();
		foreach (array_keys ($this->config["NODES"]) as $node) {
			$this->sems[$node] = sem_get ($this->semKeys[$node]);
		}
	}

	public function removeShm () {
		if (!empty ($this->queue)) {
			if (!msg_remove_queue ($this->queue)) {
				syslog(LOG_WARNING, "Error removing queue key ".$this->queueKey);
			}
			$this->queue = NULL;
		}
	}

	/* all processes */
	public function checkFile () {
		if (empty ($this->config["CHECKFILE"])) {
			syslog(LOG_CRIT, "No CHECKFILE configured");
			return FALSE;
		}

		if (!file_exists ($this->config["CHECKFILE"])) {
			syslog(LOG_CRIT, "Checkfile ".$this->config["CHECKFILE"]." does not exist");
			return FALSE;
		}

		return TRUE;
	}

	/* manage timeout of nodes */
	public function setWaiting ($node) {
		$this->nodeTimeout[$node] = time()+$this->config["SCANTIME"];
	}

	public function setFailing ($node) {
		$this->nodeTimeout[$node] = time()+$this->config["FAILTIME"];
	}

	public function isReady ($node) {
		if (!empty ($this->nodeTimeout[$node]) && time() <= $this->nodeTimeout[$node]) {
			return FALSE;
		} else {
			$this->clearTimeout ($node);
			return TRUE;
		}
	}

	public function clearTimeout ($node) {
		unset ($this->nodeTimeout[$node]);
	}

	/* push and pull procs */
	public function doSleep ($time) {
		$usec = (int)($time*1000000);
		usleep($usec);
	}

	/* main process */
	public function run () {
		register_shutdown_function (array ($this, "shutdown"));

		if ($this->config["DRYRUN"]) {
			syslog(LOG_NOTICE, "Started in dry-run mode");
		} else {
			syslog(LOG_NOTICE, "Started");
		}

		$this->setupShmKeys ();
		$this->setupShm ();

		// fork batch creator
		$pidBatch = pcntl_fork ();
		if ($pidBatch < 0) {
			die ("Could not fork batch creator");
		} else if ($pidBatch == 0) {
			// child
			$this->enqueueLoop ();
		}

		// fork puller
		$pid = pcntl_fork ();
		if ($pid < 0) {
			die ("Could not fork puller");
		} else if ($pid == 0) {
			// child
			$this->pullLoop ("pull");
			exit(0);
		}

		// fork pushers
		for ($i=0; $i < $this->config["PUSHPROCS"]; $i++) {
			$pid = pcntl_fork ();
			if ($pid < 0) {
				die ("Could not fork worker");
			} else if ($pid == 0) {
				// child
				$this->pushLoop("push $i");
				exit(0);
			}
		}

		// handle signals for clearing shm only in main proc
		pcntl_signal(SIGINT, "sync_shutdown");
		pcntl_signal(SIGTERM, "sync_shutdown");

		$this->schedulerLoop ();
	}

	/* scheduler process */
	public function schedulerLoop () {
		$this->nextNodeId = array("push" => 0, "pull" => 0);
		$this->reopenLog("sched");
		while (TRUE) {
			$this->reloadConfig ("sched");
			if (!$this->checkFile ()) {
				$this->doSleep($this->config["FAILTIME"]);
				continue;
			}

			$sleep = $this->config["SCANTIME"];
			try {
				$toSchedule = $this->config["PUSHCOUNT"];
				$rem = $this->scheduleBatches ("push", $toSchedule);
				if ($rem < $toSchedule) {
					// we scheduled something
					$sleep = 0;
				}
				$this->waitComplete ($toSchedule - $rem);

				$toSchedule = $this->config["PULLCOUNT"];
				$rem = $this->scheduleBatches ("pull", $toSchedule);
				if ($rem < $toSchedule) {
					// we scheduled something
					$sleep = 0;
				}
				$this->waitComplete ($toSchedule - $rem);
			} catch (Exception $e) {
				syslog(LOG_CRIT, "Scheduler: ".print_r($e, TRUE));
				$sleep = $this->config["FAILTIME"];
			}

			if ($sleep) {
				$this->doSleep($sleep);
			}
		}
	}

	public function scheduleBatches ($mode, $toSchedule) {
		$curtime = time();
		$tasks = array ();
		foreach (array_keys ($this->config["NODES"]) as $node) {
			if (!$this->isReady ($node)) {
				$tasks[] = array();
				continue;
			}

			$dir = $this->config["BATCHDIR"]."/$mode/$node";
			if (!is_dir ($dir)) {
				mkdir ($dir, 0777, TRUE);
			}
			if(!is_dir($dir)){
				syslog(LOG_CRIT, "Cannot opendir $dir, will retry in ".$this->config["FAILTIME"]." seconds");
				$this->setFailing ($node);
				continue;
			}

			$allBatches = glob($dir . '/*_*.batch');
			if(empty($allBatches)){
				//nothing to do
				continue;
			}

			$nodecfg = $this->config["NODES"][$node];
			$bulkMaxBatches = !empty($nodecfg["BULK_MAX_BATCHES"]) ? $nodecfg["BULK_MAX_BATCHES"] : $this->config["BULK_MAX_BATCHES"];

			$batches = array_map('basename', array_splice($allBatches, 0, $bulkMaxBatches * 2));
			unset($allBatches);

			$rowno = 0;
			$match = $bulk = array();
			$bulkCount = 0;
			$lastType = null;
			foreach ($batches as $batch) {
				if (!preg_match ('/_([^_\.]+)\.batch$/', $batch, $match)) {
					continue;
				}
				$curType = $match[1];

				$batchFile = "$dir/$batch";
				$mtime = filemtime ($batchFile);
				if ($mtime === FALSE ||
					$curtime - $mtime < $this->config["BULK_OLDER_THAN"] ||
					$bulkCount > $bulkMaxBatches ||
					($lastType && $curType != $lastType) // rec != norec
				) {
					# flush bulk
					if ($mtime === FALSE) {
						syslog(LOG_WARN, "Cannot get mtime of $batchFile, assuming new bulk");
					}
					if ($bulkCount > 0) {
						$tasks[$rowno++][] = array($node, $bulk);
					}
					$bulk = array();
					$bulkCount = 0;
					$lastType = null;
					break; // we don't send more than one bulk per node, anyway
				}

				$bulk[] = $batch;
				$bulkCount++;
				$lastType = $curType;
			}

			if ($bulkCount > 0) {
				$tasks[$rowno++][] = array($node, $bulk);
			}
		}

		$rowno = 0;
		$taskCnt = count($tasks);
		$scheduledNodes = array ();
		// schedule the same node only once to ensure the order of batches
		while ($toSchedule > 0 && $rowno < $taskCnt) {
			$row = $tasks[$rowno];
			$rowCnt = count($row);
			/* count($row) should be the number of nodes,
			 * however we use count($row) for safety */
			for ($i=0; $i < $rowCnt && $toSchedule > 0; $i++) {
				$nodeId = $this->nextNodeId[$mode] % $rowCnt;
				$this->nextNodeId[$mode]++;
				if (!empty ($row[$nodeId])) {
					$task = $row[$nodeId];
					if (!in_array ($task[0], $scheduledNodes)) {
						$msgtype = $mode == "push" ? $this->MSG_PUSH : $this->MSG_PULL;

						if (!msg_send ($this->queue, $msgtype, $task)) {
							syslog(LOG_CRIT, "Error scheduling task ".print_r($task, true));
						} else {
							$toSchedule--;
							$scheduledNodes[] = $task[0];
						}
					}
				}
			}

			$rowno++;
		}

		return $toSchedule;
	}

	public function waitComplete ($n) {
		$msgtype = $message = $errorcode = '';
		while ($n > 0) {
			if (!msg_receive ($this->queue, $this->MSG_RESULT, $msgtype, 4096, $message, true, 0, $errorcode)) {
				syslog(LOG_CRIT, "Error waiting completion from queue: ".posix_strerror($errorcode));
			} else {
				list($node, $res) = $message;
				if (((string) $res) == "fail") {
					$this->setFailing ($node);
				}
				$n--;
			}
		}
	}

	/* batch queue loop */
	public function enqueueLoop () {
		$this->reopenLog ("batchq");
		while (TRUE) {
			$this->reloadConfig ("batchq");
			if (!$this->checkFile ()) {
				$this->doSleep($this->config["FAILTIME"]);
				continue;
			}

			try {
				if (!$this->linkLocalBatches () || !$this->pullRemoteBatches ()) {
					$this->doSleep($this->config["FAILTIME"]);
				} else {
					$this->doSleep($this->config["SCANTIME"]);
				}
			} catch (Exception $e) {
				syslog(LOG_CRIT, "Enqueue loop: ".print_r($e, TRUE));
				$this->doSleep($this->config["FAILTIME"]);
			}
		}
	}

	/* enqueue process */
	public function linkLocalBatches () {
		$dir = $this->config["BATCHDIR"];
		$batches = scandir ($dir, 0); // sort ascending
		if ($batches === FALSE) {
			syslog(LOG_CRIT, "Cannot scan $dir, will retry in ".$this->config["FAILTIME"]." seconds");
			return FALSE;
		}

		if (empty ($this->config["NODES"])) {
			return TRUE;
		}
		$match = array();
		foreach ($batches as &$batch) {
			if (!preg_match ("/^\d+_([^_]+)_.*\.batch$/", $batch, $match)) {
				/*if(!empty($this->config["LOG_DEBUG"])){
					syslog(LOG_DEBUG, "batch doesn't match " . $batch);
				}*/
				continue;
			}
			$batchNode = $match[1];

			// create hard links for each node
			foreach (array_keys ($this->config["NODES"]) as $node) {
				if ($batchNode == $node) {
					// do not push to the node where this that was pulled from
					continue;
				}

				$nodedir = "$dir/push/$node";
				if(!is_dir($nodedir)){
					mkdir($nodedir, 0777, TRUE);
				}
				while (!file_exists ("$nodedir/$batch") && !link ("$dir/$batch", "$nodedir/$batch")) {
					syslog(LOG_ALERT, "Error creating link from $dir/$batch to $nodedir/$batch, cannot continue safely");
					$this->doSleep($this->config["FAILTIME"]);
					$this->reloadConfig ("batchq");
				}

				if (!empty ($this->config["BACKUPBATCHES"])) {
					$bakdir = $this->config["BACKUPBATCHES"]."/".strftime("%F")."/push/$node/";
					while (!is_dir ($bakdir) && !mkdir ($bakdir, 0777, TRUE)) {
						syslog(LOG_ALERT, "Error creating backup directory $bakdir, cannot continue safely");
						$this->doSleep($this->config["FAILTIME"]);
						$this->reloadConfig ("batchq");
					}

					while (!file_exists ("$bakdir/$batch") && !link ("$dir/$batch", "$bakdir/$batch")) {
						syslog(LOG_ALERT, "Error creating link from $dir/$batch to $bakdir/$batch, cannot continue safely");
						$this->doSleep($this->config["FAILTIME"]);
						$this->reloadConfig ("batchq");
					}
				}
			}

			if (!unlink ("$dir/$batch")) {
				syslog(LOG_CRIT, "Could not unlink $dir/$batch, will be retried");
			}
		}

		return TRUE;
	}

	/* enqueue process */
	public function pullRemoteBatches () {
		foreach (array_keys ($this->config["NODES"]) as $node) {
			if (!$this->isReady ($node)) {
				continue;
			}
			if (!$this->pullBatches ($node)) {
				$this->setFailing ($node);
			}
		}
		return TRUE;
	}

	/* enqueue process */
	public function pullBatches ($node) {
		if (empty ($this->config["PULL_BATCHES"]) || empty($this->config["NODES"][$node]["BATCHES"])) {
			return TRUE;
		}

		$dir = $this->config["BATCHDIR"]."/pull/$node";
		if(!is_dir($dir)){
			mkdir ($dir, 0777, TRUE);
		}

		$command = $this->config["PULL_BATCHES"];
		$subst = array ("%s" => $this->config["NODES"][$node]["BATCHES"],
						"%d" => $dir);
		if (!$this->executeCommand ($command, $subst)) {
			syslog(LOG_WARNING, "Pull batches from $node failed, will retry in ".$this->config["FAILTIME"]." seconds");
			return FALSE;
		}

		if (!empty($this->config["LOG_DEBUG"])) {
			syslog (LOG_DEBUG, "Pull batches from $node succeeded");
		}

		return TRUE;
	}

	/* pull process */
	public function pullLoop ($ident) {
		$this->reopenLog ($ident);
		$msgtype = $message = $errorcode = '';
		while (TRUE) {
			$this->reloadConfig ($ident);
			try {
				if (!msg_receive ($this->queue, $this->MSG_PULL, $msgtype, ESTIMATED_BATCH_NAME_LENGTH*$this->config["BULK_MAX_BATCHES"], $message, true, 0, $errorcode)) {
					syslog(LOG_CRIT, "[pull] Cannot pop from queue, putting worker in sleep: ".posix_strerror($errorcode));
					$this->doSleep($this->config["FAILTIME"]);
					continue;
				}
				list($node, $batches) = $message;
				$dir = $this->config["BATCHDIR"]."/pull/$node";
				if(!is_dir($dir)){
					mkdir ($dir, 0777, TRUE);
				}

		if (!$this->syncDataBatch ($node, $batches, "pull")) {
					msg_send ($this->queue, $this->MSG_RESULT, array($node, "fail"));
					continue;
				}

				if (!empty ($this->config["BACKUPBATCHES"])) {
					$bakdir = $this->config["BACKUPBATCHES"]."/".strftime("%F")."/pull/$node";
					while (!is_dir ($bakdir) && !mkdir ($bakdir, 0777, TRUE)) {
						syslog(LOG_ALERT, "Error creating backup directory $bakdir, cannot continue safely");
						$this->doSleep($this->config["FAILTIME"]);
						$this->reloadConfig ($ident);
					}

					foreach ($batches as $batch) {
						$batchFile = "$dir/$batch";
						while (!file_exists ("$bakdir/$batch") && !link ($batchFile, "$bakdir/$batch")) {
							syslog(LOG_ALERT, "Error creating backup link from $batchFile to $bakdir/$batch, cannot continue safely");
							$this->doSleep($this->config["FAILTIME"]);
							$this->reloadConfig ($ident);
						}
					}
					unset($batchFile);
				}

				if (!$this->config["DRYRUN"]) {
					// move successful batches
					foreach ($batches as $batch) {
						$batchFile = "$dir/$batch";
						$destFile = $this->config["BATCHDIR"]."/$batch";
						if (!rename ($batchFile, $destFile)) {
							syslog(LOG_CRIT, "Cannot move pulled batch $batchFile into ".$destFile.", the batch will be retried");
							msg_send ($this->queue, $this->MSG_RESULT, array($node, "fail"));
							continue 2;
						}
					}
					unset($batchFile);
				}
				msg_send ($this->queue, $this->MSG_RESULT, array($node, "success"));
			} catch (Exception $e) {
				syslog(LOG_CRIT, "Worker loop: ".print_r($e, TRUE));
				$this->doSleep($this->config["FAILTIME"]);
			}
		}
	}

	/* push process */
	public function pushLoop ($ident) {
		$this->reopenLog ($ident);
		$msgtype = $message = $errorcode = '';
		while (TRUE) {
			$this->reloadConfig ($ident);
			try {
				if (!msg_receive ($this->queue, $this->MSG_PUSH, $msgtype, ESTIMATED_BATCH_NAME_LENGTH*$this->config["BULK_MAX_BATCHES"], $message, true, 0, $errorcode)) {
					syslog(LOG_CRIT, "[push] Cannot pop from queue, sleeping: ".posix_strerror($errorcode));
					$this->doSleep($this->config["FAILTIME"]);
					continue;
				}

				list($node, $batches) = $message;
				$dir = $this->config["BATCHDIR"]."/push/$node";

				if(!is_dir($dir)){
					mkdir ($dir, 0777, TRUE);
				}
				$res = FALSE;

				while (!sem_acquire ($this->sems[$node])) {
					syslog(LOG_INFO, "Cannot acquire lock for $node, retrying in ".$this->config["SCANTIME"]);
					$this->doSleep($this->config["FAILTIME"]);
				}

				try {
					if ($this->syncDataBatch ($node, $batches, "push")) {
						$res = TRUE;
					}
				} catch (Exception $e) {
					syslog(LOG_CRIT, "Error while syncing $node $batches: ".print_r($e));
					$res = FALSE;
				}

				if (!sem_release ($this->sems[$node])) {
					syslog(LOG_CRIT, "Could not release lock for $node");
				}

				if ($res && !$this->config["DRYRUN"]) {
					// unlink from push dir
					foreach ($batches as $batch) {
						$batchFile = "$dir/$batch";
						if (!unlink ($batchFile)) {
							syslog(LOG_CRIT, "Could not unlink $batchFile, will be retried");
							$res = FALSE;
						}
					}
					unset($batchFile);
				}

				msg_send($this->queue, $this->MSG_RESULT, array($node, ($res ? "success" : "fail")));
			} catch (Exception $e) {
				syslog(LOG_CRIT, "Worker loop: ".print_r($e, TRUE));
				$this->doSleep($this->config["FAILTIME"]);
			}
		}
	}

	/* pull or push process */
	public function syncDataBatch ($node, $batches, $mode) {
		if (!empty($this->config["LOG_DEBUG"])) {
			syslog(LOG_DEBUG, "Process batches ".print_r($batches, true)." for node $node");
		}

		if (empty ($this->config["NODES"][$node])) {
			syslog(LOG_CRIT, "Empty configuration for $node, will retry batches ".print_r($batches, true)." in ".$this->config["FAILTIME"]." seconds");
			return FALSE;
		}

		$batchesType = null;
		$dir = $this->config["BATCHDIR"]."/$mode/$node";
		// verify valid batch filenames
		$match = array();
		foreach ($batches as $batch) {
			$batchFile = "$dir/$batch";
			if (!preg_match ("/_([^_\.]+)\.batch$/", $batchFile, $match)) {
				syslog(LOG_ALERT, "Invalid batch filename format $batchFile");
				return FALSE;
			}
			$batchesType = $match[1];
		}

		$batchFile = null;
		$input = null;
		if (count($batches) == 1) {
			$batch = $batches[0];
			$batchFile = "$dir/$batch";
		} else {
			$batchFile = "-";
			$tmp = array();
			// read batch contents
			foreach ($batches as $batch) {
				$curBatchFile = "$dir/$batch";
				$tmp = array_merge($tmp, explode("\n", trim(file_get_contents($curBatchFile))));
			}
			//if this will generate one transaction, we don't need duplicates & the order should not
			$input = implode("\n", array_unique($tmp)) . "\n";
		}

		$nodecfg = $this->config["NODES"][$node];

		$command = ($batchesType == "rec" ?
				(empty($nodecfg["SYNC_DATA_REC"]) ? $this->config["SYNC_DATA_REC"] : $nodecfg["SYNC_DATA_REC"]) :
				(empty($nodecfg["SYNC_DATA_NOREC"]) ? $this->config["SYNC_DATA_NOREC"] : $nodecfg["SYNC_DATA_NOREC"])
			);

		$subst = array ("%b" => $batchFile,
						"%s" => ($mode == "push") ? $this->config["DATADIR"] : $nodecfg["DATA"],
						"%d" => ($mode == "push") ? $nodecfg["DATA"] : $this->config["DATADIR"]);
		if (!$this->executeCommand ($command, $subst, $input)) {
			syslog(LOG_WARNING, "Batch $mode execution failed, will retry batches ".print_r($batches, true)." in ".$this->config["FAILTIME"]." seconds");
			return FALSE;
		}

		if (!empty($this->config["LOG_DEBUG"])) {
			syslog (LOG_DEBUG, "Batches $mode ".print_r($batches, true)." succeeded");
		}
		return TRUE;
	}

	/* node process */
	function executeCommand ($command, $subst, $input=null) {
		if (!$this->checkFile ()) {
			return FALSE;
		}

		foreach ($subst as $k => $v) {
			$command = str_replace ($k, escapeshellcmd($v), $command);
		}
		if (!empty($this->config["LOG_DEBUG"])) {
			syslog(LOG_DEBUG, "Executing command $command inputsize:".  ($input?strlen($input):0));
		}

		if ($this->config["DRYRUN"]) {
			return TRUE;
		}

		$desc = array(
			2 => array('pipe', 'w')
		);

		//we don't need the output of sync in normal operation mode, just print it in debug mode
		if(empty($this->config["LOG_DEBUG"])){
			$desc[1] = array("file", "/dev/null", "w");
		}

		if ($input) {
			$desc[0] = array('pipe', 'r');
		}

		$pipes = array();
		// spawn the process
		$p = proc_open($command, $desc, $pipes);
		if (!is_resource ($p)) {
			syslog(LOG_WARNING,"unable to execute ".$command);
			return FALSE;
		}
		if($input){
			stream_set_blocking($pipes[0], 0);
		}
		stream_set_blocking($pipes[2], 0);

		$tx = $input ? true : false;
		$err = '';
		$inputLen = $input ? strlen($input) : 0;
		$inputPos = 0;
		$tmp = '';
		while(($status = proc_get_status($p)) && $status['running'] == true){
			if($tx && $inputPos != $inputLen){
				$inputPos+=fwrite($pipes[0], substr($input, $inputPos, 8192));
			} elseif($tx) {
				$tx = false;
				fclose($pipes[0]);
			}
			$tmp = fgets($pipes[2], 1024);
			//wait if nothing read - but only if we don't have data to write
			if(!$inputLen && strlen($tmp) === 0){
				usleep(10 * 1000);
			} else {
				$err.=$tmp;
			}
		}
		if(strlen($input) != $inputPos){
			syslog(LOG_CRIT, 'WARNING LENGTH MISSMATCH ' . strlen($input) . " -> $inputPos ");
		}
		foreach($pipes as $pipe){
			if(is_resource($pipe)){
				fclose($pipe);
			}
		}

		if (in_array ($status['exitcode'], $this->config["ACCEPT_STATUS"])) {
			return TRUE;
		}
		syslog(LOG_CRIT, "Command '$command', status ".$status['exitcode'].", inputsize: " . strlen($input) . " stderr: $err");
		return FALSE;
	}
}

$opts = getopt("c:p:a:u:g:h");
if ($opts === FALSE) {
	exit(1);
}

if (!empty($opts["h"])) {
	echo "Usage: {$argv[0]} [ -c config.php ] [ -p sync.pid ] [ -u uid ] [ -g gid ] [ -a start|stop ]\n";
	exit(0);
}

$configPath = isset($opts["c"]) ? $opts["c"] : dirname(__FILE__)."/config.php";

$sync = new Sync ($configPath, !empty($opts["p"]) ? $opts["p"] : null);
//have config for UID/GID
require $configPath;

//direct parameter has precedence over config var
$gid = intval(empty($opts["g"]) ? (empty($GID) ? 0 : $GID) : $opts["g"]);
if($gid){
	if(posix_setgid($gid) === FALSE){
		syslog(LOG_CRIT, "Could not setgid to $gid");
		exit(1);
	}
}

//direct parameter has precedence over config var
$uid = intval(empty($opts["u"]) ? (empty($UID) ? 0 : $UID) : $opts["u"]);
if($uid){
	if(posix_setuid($uid) === FALSE){
		syslog(LOG_CRIT, "Could not setuid to $uid");
		exit(1);
	}
}

$oldpid = 0;
if (!empty ($opts["p"])) {
	$pidPath = $opts["p"];
	$oldpid = (int) is_readable($pidPath) ? file_get_contents($pidPath) : 0;
}

if (!empty($opts["a"]) && $opts["a"] == "stop") {
	if (empty ($opts["p"])) {
		syslog(LOG_CRIT, "Must specify the pidfile with -p");
		exit(1);
	}

	if ($oldpid > 0) {
		$pgid = posix_getpgid($oldpid);
		if ($pgid > 0) {
			if (posix_kill (-$pgid, SIGTERM) === FALSE) {
				syslog(LOG_CRIT, "Could not kill $oldpid");
				exit(1);
			}
		}
	}

	exit(0);
}

if (!empty($opts["a"]) && $opts["a"] != "start") {
	syslog(LOG_CRIT, "Action must be either start or stop");
	exit(1);
}

if ($oldpid > 0 && file_exists ("/proc/$oldpid")) {
	syslog(LOG_CRIT, "Process already started: $oldpid");
	exit(1);
}

if (!empty($opts["p"])) {
	$pidPath = $opts["p"];

	$pidfile = fopen($pidPath, 'c+');
	if (!$pidfile) {
		syslog(LOG_CRIT, "Cannot open pid file $pidPath");
		exit(1);
	}

	fseek($pidfile, 0);
	ftruncate($pidfile, 0);
	fwrite($pidfile, posix_getpid());
	fflush($pidfile);
	fclose($pidfile);
}

$sync->run ();

exit(0);
