let search_table = document.getElementById("search-table");
let status_table = document.getElementById("status-table");

// yoink from https://stackoverflow.com/a/20463021
function fileSizeIEC(bytes) {
	if (bytes == 0) {
		return "0 B";
	}
    const exponent = Math.floor(Math.log(bytes) / Math.log(1024.0))
    const decimal = (bytes / Math.pow(1024.0, exponent)).toFixed(exponent ? 2 : 0)
    return `${decimal} ${exponent ? `${'kMGTPEZY'[exponent - 1]}iB` : 'B'}`
}

// straight out of the MDN docs fetch() example
async function updateStatus() {
	const url = "status";
	const status_tbody = document.getElementById("status-table-body");
	try {
		const response = await fetch(url);
		if (!response.ok) {
			throw new Error(`Response contains error: ${response.status}`);
		}
		const result = await response.json();
		const data = result.data;
		for (let i = 0; i < data.length; i += 1) {
			let row = status_tbody.insertRow();
			row.insertCell(0).innerText = data[i].hub;
			row.insertCell(1).innerText = data[i].addr;
			row.insertCell(2).innerText = data[i].users;
			row.insertCell(3).innerText = fileSizeIEC(data[i].shared_size);
			if (data[i].active) {
				row.insertCell(4).innerText = "Active";
			} else {
				row.insertCell(4).innerText = "Down";
			}
		}
	} catch (error) {
		console.error(error.message);
	}
}

async function updateSearch(needle) {
	const search_tbody = document.getElementById("search-table-body");
	const search_details = document.getElementById("search-details");

	search_tbody.innerHTML = "";

	const url = `search/?p=${needle}`;
	search_details.innerText = "Fetching results..."

	try {
		const response = await fetch(url);
		if (!response.ok) {
			throw new Error(`Response contains error: ${response.status}`);
		}
		const result = await response.json();
		const data = result.data;
		search_details.innerText = `${data.length - 1} results found`
		for (let i = 0; i < data.length; i += 1) {
			if (Object.keys(data[i]).length === 0) {
				break;
			}
			let row = search_tbody.insertRow();
			row.insertCell(0).innerText = i + 1;
			row.insertCell(1).innerText = data[i].Hub;
			row.insertCell(2).innerText = data[i].user;

			let d3 = row.insertCell(3);
			d3.innerText = data[i].path;
			d3.style = "font-family: monospace;";
		}
	} catch (error) {
		console.error(error.message);
		search_details.innerText = "Errored out!"
	}
}

updateStatus();
